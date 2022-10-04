/*
Developed by Sandeep Sharma with contributions from James E. Smith and Adam A. Homes, 2017
Copyright (c) 2017, Sandeep Sharma

This file is part of DICE.
This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "global.h"
#include <stdlib.h>
#include <stdio.h>
#include <fstream>
#include "Determinants.h"
#include "SHCIgetdeterminants.h"
#include "SHCISortMpiUtils.h"
#include "SHCImakeHamiltonian.h"
#include "integral.h"
#include "Hmult.h"
#include "Davidson.h"
#include <Eigen/Dense>
#include <Eigen/Core>
#include <set>
#include <list>
#include <tuple>
#include <numeric>
#include <boost/serialization/shared_ptr.hpp>
#ifndef SERIAL
#include <boost/mpi/environment.hpp>
#include <boost/mpi/communicator.hpp>
#include <boost/mpi.hpp>
#endif
#include <boost/serialization/vector.hpp>
#include "communicate.h"
#include <boost/interprocess/managed_shared_memory.hpp>
#include "SHCIshm.h"
#include <boost/format.hpp>
#include <boost/algorithm/string.hpp>

using namespace Eigen;
using namespace boost;
using namespace SHCISortMpiUtils;
int HalfDet::norbs = 1; //spin orbitals
int Determinant::norbs = 1; //spin orbitals
int Determinant::EffDetLen = 1;
char Determinant::Trev = 0; //Time reversal
Eigen::Matrix<size_t, Eigen::Dynamic, Eigen::Dynamic> Determinant::LexicalOrder ;

//get the current time
double getTime() {
  struct timeval start;
  gettimeofday(&start, NULL);
  return start.tv_sec + 1.e-6*start.tv_usec;
}
double startofCalc = getTime();


void license() {
  pout << endl;
  pout << endl;
  pout << "**************************************************************"<<endl;
  pout << "Dice  Copyright (C) 2017  Sandeep Sharma"<<endl;
  pout <<"This program is distributed in the hope that it will be useful,"<<endl;
  pout <<"but WITHOUT ANY WARRANTY; without even the implied warranty of"<<endl;
  pout <<"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE."<<endl;  
  pout <<"See the GNU General Public License for more details."<<endl;
  pout << endl<<endl;
  pout << "Author:       Sandeep Sharma"<<endl;
  pout << "Contributors: James E Smith, Adam A Holmes, Bastien Mussard"<<endl;
  pout << "For detailed documentation on Dice please visit"<<endl;
  pout << "https://sanshar.github.io/Dice/"<<endl;
  pout << "Please visit our group page for up to date information on other projects"<<endl;
  pout << "http://www.colorado.edu/lab/sharmagroup/"<<endl;
  pout << "**************************************************************"<<endl;
  pout << endl;
  pout << endl;
}

CItype calcZerothGreensFunction(int i, int j, Determinant* Dets, CItype* ci, int DetsSize, 
        Determinant* DetsNm1, int DetsNm1Size, Hmult2& HNm1, double E0, CItype w, twoInt& I2, oneInt& I1, double coreE, int type) ; 

//CItype calcFirstGreensFunction(int i, int j, Determinant* Dets, CItype* ci, int DetsSize, 
//        Determinant* DetsNm1, int DetsNm1Size, Hmult2& HNm1, std::vector<Determinant>& DetsPert, std::vector<CItype>& ciPert, double E0, CItype w) ; 

CItype calcGreensFunctionExact(int i, int j, Determinant* Dets, CItype* ci, int DetsSize, 
        Determinant* DetsNm1v, int DetsNm1vSize, std::vector<MatrixXd>& ciNm1, 
        std::vector<size_t>& idx, double E0, std::vector<double>& ENm1, CItype w) ; 

// class to store input
class schedule{
public:
  bool exact, all; // all: evaluate all matrix elements
  int i, j; // i and j in G(i, j, w)
  int type; // ip: -1, ea: +1; pol (not implemented): 0
  double w1, w2, eta, dw; // w1, w2: endpoints of the interval, eta: imaginary shift, dw: spacing
  
  // default constructor
  schedule() 
  {
      exact = false;
      all = false;
      type = -1;
      i = 0; j = 0;
      w1 = -2.; w2 = 2.; eta = 0.01; dw = 0.02;
  }
  
  // read input
  void readInput(std::string input) 
  {
    std::ifstream dump(input.c_str());
    while (dump.good()) {
      std::string Line;
      std::getline(dump, Line);
      trim(Line);
      std::vector<string> tok;
      boost::split(tok, Line, is_any_of(", \t\n"), token_compress_on);
      std::string ArgName = *tok.begin();
      if (!ArgName.empty() && (boost::iequals(tok[0].substr(0,1), "#"))) continue;
      else if (ArgName.empty()) continue;
      else if (boost::iequals(ArgName, "exact")) exact = true;
      else if (boost::iequals(ArgName, "i")){
        i = std::atoi(tok[1].c_str());
      }
      else if (boost::iequals(ArgName, "j")){
        j = std::atoi(tok[1].c_str());
      }
      else if (boost::iequals(ArgName, "w1")){
        w1 = std::atof(tok[1].c_str());
      }
      else if (boost::iequals(ArgName, "w2")){
        w2 = std::atof(tok[1].c_str());
      }
      else if (boost::iequals(ArgName, "eta")){
        eta = std::atof(tok[1].c_str());
      }
      else if (boost::iequals(ArgName, "dw")){
        dw = std::atof(tok[1].c_str());
      }
      else if (boost::iequals(ArgName, "type")){
        if (tok[1] == "ip") type = -1;
        else if (tok[1] == "ea") type = 1;
      }
    }
  }
} schd;


int main(int argc, char* argv[]) {

#ifndef SERIAL
  // initialize mpi environment
  boost::mpi::environment env(argc, argv);
  boost::mpi::communicator world;
#endif

  // initialize shared memory segments
  initSHM();

  //license();

  //std::cout.precision(15);
  // read input
  std::string inputFile = "green.dat";
  if (argc > 1) inputFile = std::string(argv[1]);
  schd.readInput(inputFile);

  if (schd.i % 2 != schd.j % 2) {
    pout << "i and j have different sz, G_ij = 0\n";
    exit(0);
  }

  // read the hamiltonian (integrals, orbital irreps, num-electron etc.)
  twoInt I2; oneInt I1; int nelec; int norbs; double coreE = 0.0, eps;
  std::vector<int> irrep;
  readIntegrals("FCIDUMP", I2, I1, nelec, norbs, coreE, irrep);
  pout << "# Number of electrons: " << nelec << ", number of orbitals: " << norbs << endl; 

  // setup the lexical table for the determinants
  norbs *= 2;
  Determinant::norbs = norbs; // spin orbitals
  HalfDet::norbs = norbs; // spin orbitals
  Determinant::EffDetLen = norbs/64+1;
  Determinant::initLexicalOrder(nelec);
  if (Determinant::EffDetLen > DetLen) {
    pout << "change DetLen in global.h to "<<Determinant::EffDetLen<<" and recompile "<<endl;
    exit(0);
  }

  // read dets and ci coeffs of the variational result psi0
  char file [5000];
  sprintf(file, "%d-variational.bkp" , 0);
  std::ifstream ifs(file, std::ios::binary);
  boost::archive::binary_iarchive load(ifs);
  // Dets has deteminants in psi0, ci has the corresponding coeffs
  int iter; std::vector<Determinant> Dets; std::vector<MatrixXd> ciReal; std::vector<double> E0;
  load >> iter >> Dets;
  load >> ciReal;
  load >> E0;
  ifs.close();
  E0[0] += coreE;
  MatrixXx ci = ciReal[0];
  ciReal.clear(); 
  pout << "# Variational ground state energy: " << E0[0] << endl; 
  
  double t1 = 0., t2 = 0., t3 = 0.;
  
  if (schd.exact) { 
//  exact ea gf to be defined
//    // read dets and ci coeffs of the variational result psiNm1
//    char fileNm1 [5000];
//    sprintf(fileNm1, "%d-variationalNm1.bkp" , 0);
//    std::ifstream ifsNm1(fileNm1, std::ios::binary);
//    boost::archive::binary_iarchive load(ifsNm1);
//    // DetsNm1v has deteminants in the n-1 electron variational wf, ciNm1 has the corresponding coeffs
//    int iterNm1; std::vector<Determinant> DetsNm1v; std::vector<MatrixXd> ciNm1; std::vector<double> ENm1;
//    load >> iter >> DetsNm1v;
//    load >> ciNm1;
//    load >> ENm1;
//    ifsNm1.close();
//   
//    // sorting dets
//    std::vector<Determinant> sortedDetsNm1v;
//    std::vector<size_t> idx(DetsNm1v.size());
//    if (commrank == 0) {
//      std::iota(idx.begin(), idx.end(), 0);
//      std::sort(idx.begin(), idx.end(), [&DetsNm1v](size_t i1, size_t i2){return DetsNm1v[i1] < DetsNm1v[i2];});
//      // now 'sorting' DetsNm1v
//      for(int i = 0; i < DetsNm1v.size(); i++) sortedDetsNm1v.push_back(DetsNm1v[idx[i]]); 
//    }
//    DetsNm1v.clear();
//
//#ifndef SERIAL
//  mpi::broadcast(world, idx, 0);
//#endif
//
//    // put dets in shared memory
//    Determinant* SHMDets, *SHMDetsNm1; CItype *SHMci;
//    int DetsSize = Dets.size(), DetsNm1vSize = sortedDetsNm1v.size();
//    SHMVecFromVecs(Dets, SHMDets, shciDetsCI, DetsCISegment, regionDetsCI);
//    Dets.clear();
//    SHMVecFromVecs(sortedDetsNm1v, SHMDetsNm1, shciDetsNm1, DetsNm1Segment, regionDetsNm1);
//    sortedDetsNm1v.clear();
//    SHMVecFromMatrix(ci, SHMci, shcicMax, cMaxSegment, regioncMax);
//    //ci.clear(); // need to fix this
//  
//#ifndef SERIAL
//  mpi::broadcast(world, DetsNm1vSize, 0);
//#endif
//
//    // calculate greens function
//    pout << "w               g" << schd.i << schd.j << endl; 
//    for(int i = 0; i < (schd.w2 - schd.w1)/schd.dw; i++){
//        std::complex<double> w (schd.w1 + schd.dw*i, schd.eta);
//        CItype g_ij = calcGreensFunctionExact(schd.i, schd.j, SHMDets, SHMci, DetsSize, SHMDetsNm1, DetsNm1vSize, ciNm1, idx, E0[0], ENm1, w);
//        t3 = MPI_Wtime();
//        pout << w << "   " << g_ij << endl;
//    } 
  }
  else { // not exact
    // to store dets in the response space, to construct H0 in the response space 
    // calc only on root
    std::vector<Determinant> DetsResponse; 
    std::vector<Determinant> SortedDetsvec; // dets in psi0 sorted 
 
    if (commrank == 0) {
      // fill up DetsResponse
      if (schd.type == -1) {
        for(int k = 0; k < Dets.size(); k++) {
          // get the occupied orbs in Dets[k]
          std::vector<int> closed(64*DetLen);
          int nclosed = Dets[k].getClosed(closed);
          
          // remove electrons from occupied orbs
          for(int l = 0; l < nclosed; l++){
            if (closed[l] % 2 == schd.i % 2) {
              Dets[k].setocc(closed[l], false);
              DetsResponse.push_back(Dets[k]);
              Dets[k].setocc(closed[l], true);
            }
          }
        }
      }
      else if (schd.type == 1) {
        for(int k = 0; k < Dets.size(); k++) {
          // get the occupied orbs in Dets[k]
          std::vector<int> open(64*DetLen);
          int nopen = Dets[k].getOpen(open);
          
          // add electrons to empty orbs
          for(int l = 0; l < nopen; l++){
            if (open[l] % 2 == schd.i % 2) {
              Dets[k].setocc(open[l], true);
              DetsResponse.push_back(Dets[k]);
              Dets[k].setocc(open[l], false);
            }
          }
        }
      
      }
      
      // remove duplicates
      std::sort(DetsResponse.begin(), DetsResponse.end());
      SHCISortMpiUtils::RemoveDuplicates(DetsResponse);
      for (int i = 0; i < Dets.size(); i++) SortedDetsvec.push_back(Dets[i]);
      std::sort(SortedDetsvec.begin(), SortedDetsvec.end());
    }

#ifndef SERIAL
  MPI_Barrier(MPI_COMM_WORLD);
#endif
  
    // put dets in shared memory
    Determinant* SHMDets, *SHMDetsResponse, *SHMSortedDets; CItype *SHMci; // don't need non-sorted shared dets, delete later
    int DetsSize = Dets.size(), DetsResponseSize = DetsResponse.size();
    SHMVecFromVecs(Dets, SHMDets, shciDetsCI, DetsCISegment, regionDetsCI);
    Dets.clear();
    SHMVecFromVecs(SortedDetsvec, SHMSortedDets, shciSortedDets, SortedDetsSegment, regionSortedDets);
    SortedDetsvec.clear();
    SHMVecFromVecs(DetsResponse, SHMDetsResponse, shciDetsResponse, DetsResponseSegment, regionDetsResponse);
    DetsResponse.clear();
    SHMVecFromMatrix(ci, SHMci, shcicMax, cMaxSegment, regioncMax);
    //ci.clear(); need to fix this
    pout << "# Size of the original electron space: " << DetsSize << endl;
    pout << "# Size of the response space: " << DetsResponseSize << endl;

#ifndef SERIAL
    mpi::broadcast(world, DetsResponseSize, 0);
#endif

    // make H0 in the response space
    SHCImakeHamiltonian::HamHelpers2 helper2;
    SHCImakeHamiltonian::SparseHam sparseHam;
    int Norbs = 2.*I2.Direct.rows();
    
    if (commrank == 0) {
      helper2.PopulateHelpers(SHMDetsResponse, DetsResponseSize, 0);
    }	
    helper2.MakeSHMHelpers();
    t1 = MPI_Wtime();
    sparseHam.makeFromHelper(helper2, SHMDetsResponse, 0, DetsResponseSize, Norbs, I1, I2, coreE, false);
    Hmult2 H(sparseHam);
    t2 = MPI_Wtime();

/*
  This part is for getting the PT dets
  // preparing first order perturbation calculation
  // initialize the heatbath integral
  std::vector<int> allorbs;
  for (int i=0; i<norbs/2; i++) allorbs.push_back(i);
  twoIntHeatBath I2HB(1.e-10);
  twoIntHeatBathSHM I2HBSHM(1.e-10);
  if (commrank == 0) I2HB.constructClass(allorbs, I2, I1, norbs/2);
  I2HBSHM.constructClass(norbs/2, I2HB);

  //testing getdets
  //int nclosed = nelec;
  //int nopen   = norbs-nclosed;
  //vector<int> closed(nelec,0);
  //vector<int> open(norbs-nelec,0);
  //Determinant d = SHMDets[0];
  //d.getOpenClosed(open, closed);
  ////d.getRepArray(detArray);

  //// mono-excited determinants
  //pout << "singly excited" << endl;
  //for (int ia=0; ia<nopen*nclosed; ia++){
  //  int i=ia/nopen, a=ia%nopen;
  //  //CItype integral = d.Hij_1Excite(closed[i],open[a],int1,int2);
  //  CItype integral = Hij_1Excite(open[a],closed[i],I1,I2, &closed[0], nclosed);

  //  // sgn
  //  if (closed[i]%2 != open[a]%2) {
  //    double sgn = 1.0;
  //    d.parity(min(open[a],closed[i]), max(open[a],closed[i]),sgn);
  //    integral = I1(open[a], closed[i])*sgn;
  //  }

  //  // generate determinant if integral is above the criterion
  //  if (fabs(integral) > 1e-10 ) {
  //    Determinant di = d;
  //    di.setocc(open[a], true); di.setocc(closed[i],false);
  //    pout << di << endl;
  //  }
  //} // ia

  //
  //// bi-excitated determinants
  ////#pragma omp parallel for schedule(dynamic)
  ////if (fabs(I2.maxEntry) < 1e-10) return;
  //// for all pairs of closed
  //pout << "double excited" << endl;
  //for (int ij=0; ij<nclosed*nclosed; ij++) {
  //  int i=ij/nclosed, j = ij%nclosed;
  //  if (i<=j) continue;
  //  int I = closed[i]/2, J = closed[j]/2;
  //  int X = max(I, J), Y = min(I, J);

  //  int pairIndex = X*(X+1)/2+Y;
  //  size_t start = closed[i]%2==closed[j]%2 ? I2HBSHM.startingIndicesSameSpin[pairIndex]   : I2HBSHM.startingIndicesOppositeSpin[pairIndex];
  //  size_t end   = closed[i]%2==closed[j]%2 ? I2HBSHM.startingIndicesSameSpin[pairIndex+1] : I2HBSHM.startingIndicesOppositeSpin[pairIndex+1];
  //  float* integrals  = closed[i]%2==closed[j]%2 ?  I2HBSHM.sameSpinIntegrals : I2HBSHM.oppositeSpinIntegrals;
  //  short* orbIndices = closed[i]%2==closed[j]%2 ?  I2HBSHM.sameSpinPairs     : I2HBSHM.oppositeSpinPairs;
  //  pout << start << "  " << end << endl;

  //  // for all HCI integrals
  //  for (size_t index=start; index<end; index++) {
  //    // if we are going below the criterion, break
  //    //if (fabs(integrals[index]) < 1e-10) break;

  //    // otherwise: generate the determinant corresponding to the current excitation
  //    int a = 2* orbIndices[2*index] + closed[i]%2, b= 2*orbIndices[2*index+1]+closed[j]%2;
  //    if (!(d.getocc(a) || d.getocc(b))) {
  //      Determinant di = d;
  //      di.setocc(a, true), di.setocc(b, true), di.setocc(closed[i],false), di.setocc(closed[j], false);
  //      pout << di << endl;
  //    }
  //  } // heatbath integrals
  //} // ij

  //calculating psi1
  StitchDEH uniqueDEH;
  int size = commsize, rank = commrank;
  vector<size_t> all_to_all(size*size,0);
  int count = 0;
  for (int i=0; i<DetsSize; i++) {
    if ((i%size != rank)) continue;
    SHCIgetdeterminants::getDeterminantsDeterministicPT(SHMDets[i], 0.0, SHMci[i], 0.0,
  						  I1, I2, I2HBSHM, irrep, coreE, E0[0],
  						  *uniqueDEH.Det,
  						  *uniqueDEH.Num,
  						  *uniqueDEH.Energy,
  						  schd,0, nelec);
        //pout << i << "  " << SHMDets[i] << endl;
        //pout << "connected to Dets[i]  " << endl;
        //for(int j=count; j<uniqueDEH.Det->size(); j++){
        //    pout << uniqueDEH.Det->at(j) << endl;
        //    count++;
        //}  
  }
    

  if(commsize >1 ) {
    boost::shared_ptr<vector<Determinant> >& Det = uniqueDEH.Det;
    boost::shared_ptr<vector<CItype> >& Num = uniqueDEH.Num;
    boost::shared_ptr<vector<double> >& Energy = uniqueDEH.Energy;
    boost::shared_ptr<vector<int > >& var_indices = uniqueDEH.var_indices_beforeMerge;
    boost::shared_ptr<vector<size_t > >& orbDifference = uniqueDEH.orbDifference_beforeMerge;
 
    std::vector<size_t> hashValues(Det->size());
    
    std::vector<size_t> all_to_all_cumulative(size,0);
    for (int i=0; i<Det->size(); i++) {
      hashValues[i] = Det->at(i).getHash();
      all_to_all[rank*size+hashValues[i]%size]++; 
    }
    for (int i=0; i<size; i++)
      all_to_all_cumulative[i] = i == 0 ? all_to_all[rank*size+i] :  all_to_all_cumulative[i-1]+all_to_all[rank*size+i];
    
    size_t dsize = Det->size() == 0 ? 1 : Det->size();
    vector<Determinant> atoaDets(dsize);
    vector<CItype> atoaNum(dsize);
    vector<double> atoaE(dsize);
    vector<int > atoaVarIndices;
    vector<size_t > atoaOrbDiff;

#ifndef SERIAL
    vector<size_t> all_to_allCopy = all_to_all;
    MPI_Allreduce( &all_to_allCopy[0], &all_to_all[0], 2*size*size, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
#endif    

    vector<size_t> counter(size, 0);
    for (int i=0; i<Det->size(); i++) {
      int toProc = hashValues[i]%size;
      size_t index = toProc==0 ? counter[0] : counter[toProc] + all_to_all_cumulative[toProc-1];
      
      atoaDets[index ] = Det->at(i);
      atoaNum[index] = Num->at(i);
      atoaE[index] = Energy->at(i);
      counter[toProc]++;
    }
    
    vector<int> sendcts(size,0), senddisp(size,0), recvcts(size,0), recvdisp(size,0);
    vector<int> sendctsDets(size,0), senddispDets(size,0), recvctsDets(size,0), recvdispDets(size,0);
    vector<int> sendctsVarDiff(size,0), senddispVarDiff(size,0), recvctsVarDiff(size,0), recvdispVarDiff(size,0);
    
    size_t recvSize = 0;
    for (int i=0; i<size; i++) {
      sendcts[i] = all_to_all[rank*size+i]* sizeof(CItype)/sizeof(double);
      senddisp[i] = i==0? 0 : senddisp[i-1]+sendcts[i-1];
      recvcts[i] = all_to_all[i*size+rank]*sizeof(CItype)/sizeof(double);
      recvdisp[i] = i==0? 0 : recvdisp[i-1]+recvcts[i-1];
      
      sendctsDets[i] = all_to_all[rank*size+i]* sizeof(Determinant)/sizeof(double);
      senddispDets[i] = i==0? 0 : senddispDets[i-1]+sendctsDets[i-1];
      recvctsDets[i] = all_to_all[i*size+rank]*sizeof(Determinant)/sizeof(double);
      recvdispDets[i] = i==0? 0 : recvdispDets[i-1]+recvctsDets[i-1];
      
      sendctsVarDiff[i] = all_to_all[rank*size+i];
      senddispVarDiff[i] = i==0? 0 : senddispVarDiff[i-1]+sendctsVarDiff[i-1];
      recvctsVarDiff[i] = all_to_all[i*size+rank];
      recvdispVarDiff[i] = i==0? 0 : recvdispVarDiff[i-1]+recvctsVarDiff[i-1];
      
      recvSize += all_to_all[i*size+rank];
    }
    
    recvSize =  recvSize == 0 ? 1 : recvSize;
    Det->resize(recvSize), Num->resize(recvSize), Energy->resize(recvSize);
    
#ifndef SERIAL
    MPI_Alltoallv(&atoaNum.at(0), &sendcts[0], &senddisp[0], MPI_DOUBLE, &Num->at(0), &recvcts[0], &recvdisp[0], MPI_DOUBLE, MPI_COMM_WORLD);
    MPI_Alltoallv(&atoaE.at(0), &sendctsVarDiff[0], &senddispVarDiff[0], MPI_DOUBLE, &Energy->at(0), &recvctsVarDiff[0], &recvdispVarDiff[0], MPI_DOUBLE, MPI_COMM_WORLD);
    MPI_Alltoallv(&atoaDets.at(0).repr[0], &sendctsDets[0], &senddispDets[0], MPI_DOUBLE, &(Det->at(0).repr[0]), &recvctsDets[0], &recvdispDets[0], MPI_DOUBLE, MPI_COMM_WORLD);
#endif
    uniqueDEH.Num2->clear();
    
  }
  uniqueDEH.MergeSortAndRemoveDuplicates();
  uniqueDEH.RemoveDetsPresentIn(SHMSortedDets, DetsSize);
  
  vector<Determinant>& hasHEDDets = *uniqueDEH.Det;
  vector<CItype>& hasHEDNumerator = *uniqueDEH.Num;
  vector<double>& hasHEDEnergy    = *uniqueDEH.Energy;
  for(int i=0; i<hasHEDDets.size(); i++) hasHEDNumerator[i]=hasHEDNumerator[i]/(E0[0]-hasHEDEnergy[i]);
  //pout <<" cSize  " << hasHEDDets.size() << " " << hasHEDNumerator.size() << " " << hasHEDEnergy.size() << endl; 
  //size_t orbDiff = 0;
  //for(int i=0; i< hasHEDDets.size(); i++) pout << hasHEDNumerator[i] << "  " << hasHEDDets[i] << "  " << Hij(SHMDets[0], hasHEDDets[i], I1, I2, coreE, orbDiff).real() << endl;
*/

  //constructing matrix hamiltonian to use eigen linearsolver
  //MatrixXd HamiltonianNm1=MatrixXd::Zero(DetsNm1Size, DetsNm1Size);
  //for (int i=0; i<DetsNm1Size; i++) {
  //    HamiltonianNm1(i,i) = SHMDetsNm1[i].Energy(I1, I2, coreE);
  //    for (int j=i+1; j<DetsNm1Size; j++) {
  //        if (SHMDetsNm1[i].connected(SHMDetsNm1[j])) {
  //            size_t orbDiff = 0;
  //            HamiltonianNm1(i,j) = Hij(SHMDetsNm1[i], SHMDetsNm1[j], I1, I2, coreE, orbDiff).real();
  //            HamiltonianNm1(j,i) = HamiltonianNm1(i,j);
  //        }
  //    }
  //}
  //MatrixXd Hamiltonian=MatrixXd::Zero(DetsSize, DetsSize);
  //for (int i=0; i<DetsSize; i++) {
  //    Hamiltonian(i,i) = SHMDets[i].Energy(I1, I2, coreE);
  //    pout << "Det i " << SHMDets[i] << endl;
  //    pout << "H(" << i << "," << i << ") " << Hamiltonian(i,i) << endl;
  //    for (int j=i+1; j<DetsSize; j++) {
  //        if (SHMDets[i].connected(SHMDets[j])) {
  //            size_t orbDiff = 0;
  //            Hamiltonian(i,j) = Hij(SHMDets[i], SHMDets[j], I1, I2, coreE, orbDiff).real();
  //            Hamiltonian(j,i) = Hamiltonian(i,j);
  //            pout << "H(" << i << "," << SHMDets[j]  << ") " << Hamiltonian(i,j) << endl;
  //        }
  //    }
  //}
  
    // calculate variational greens function
    pout << "# Green's function matrix element: G_" << schd.i << schd.j << endl; 
    pout << "# Using eta: " << schd.eta << endl << "#\n"; 
    pout << "#     w                            G" << endl; 
    int npoints = (schd.w2 - schd.w1) / schd.dw;
    std::vector<double> local_minima, local_maxima;
    int prevSlope = 0;
    double prevVal = 0.;
    for (int i = 0; i < npoints; i++) {
      std::complex<double> w (schd.w1 + schd.dw*i, schd.eta);
      CItype g_ij = calcZerothGreensFunction(schd.i, schd.j, SHMDets, SHMci, DetsSize, SHMDetsResponse, DetsResponseSize, H, E0[0], w, I2, I1, coreE, schd.type);
      //CItype g_ij1 = calcFirstGreensFunction(schd.ij[0], schd.ij[1], SHMDets, SHMci, DetsSize, SHMDetsNm1, DetsNm1Size, H, hasHEDDets, hasHEDNumerator, E0[0], w);
      //t3 = MPI_Wtime();
      //pout << w << "  " << g_ij << "  " << g_ij1 << "  " << " " << g_ij-((0,1.0)*g_ij1) << endl;
      pout << format(" % .3e       ( % .8e, % .8e )  \n") % w.real() % g_ij.real() % g_ij.imag(); 
      
      // check for a peak
      if (i > 0) {
        int newSlope = g_ij.imag() > prevVal ? 1 : -1;
        if (i > 1) {
          if (newSlope == 1 && prevSlope == -1) local_minima.push_back(w.real() - schd.dw);
          else if (newSlope == -1 && prevSlope == 1) local_maxima.push_back(w.real() - schd.dw);
        }
        prevSlope = newSlope;
      }
      prevVal = g_ij.imag(); 
    }
  
    if (commrank == 0 && local_minima.size() > 0) {
      pout << "# Local minima:  ";
      for (int i = 0; i < local_minima.size(); i++) printf("% 0.5e   ", local_minima[i]);
      pout << endl;
    }
    if (commrank == 0 && local_maxima.size() > 0) {
      pout << "# Local maxima:  ";
      for (int i = 0; i < local_maxima.size(); i++) printf("% 0.5e   ", local_maxima[i]);
      pout << endl;
    }
    //pout << "t_makefromhelpers " << t2 - t1 << endl;
    //pout << "t_calcgreens = " << t3 - t2 << endl;
  }
  
  removeSHM();

  return 0;
}

// variational calculation
// to calculate G^{0-}_ij(w) = < psi0 | a_i^(dag) 1/(H0-E0+w) a_j | psi0 > or  G^{0+}_ij(w) = - < psi0 | a_i^(dag) 1/(H0-E0-w) a_j | psi0 >
// w is a complex number
// psi0 is the variational ground state, a_i, a_j are annihilation operators 
// H0 is the Hamiltonian and E0 is the N electron variational ground state energy 
// Dets has dets in psi0, ci has the corresponding coeffs
// DetsResponse dets in the response space, HResponse is H0 in the response space
CItype calcZerothGreensFunction(int i, int j, Determinant* Dets, CItype* ci, int DetsSize, Determinant* DetsResponse, int DetsResponseSize, Hmult2& HResponse, double E0, CItype w, twoInt& I2, oneInt& I1, double coreE, int type) 
{
#ifndef SERIAL
  boost::mpi::communicator world;
#endif

  // to store ci coeffs in a_j | psi0 > or a_j^(dag) | psi0 >, corresponding to dets in DetsResponse
  MatrixXx ciResponse = MatrixXx::Zero(DetsResponseSize, 1);  

  // calc a_j | psi0 > or a_j^(dag) | psi0 >
  // doing this on all procs
  int nelec = Dets[0].Noccupied();
  Determinant detTemp;
  for (int k = 0; k < DetsSize; k++) {
    bool validDet = false;
    if (type == -1 && Dets[k].getocc(j)) validDet = true;
    else if (type == 1 && !Dets[k].getocc(j)) validDet = true;
    if (validDet) {
      detTemp = Dets[k];
      
      // parity : can be made faster if this becomes slow
      std::vector<int> closed(nelec);
      detTemp.getClosed(closed);
      int nelecBefore = std::lower_bound(closed.begin(), closed.begin() + nelec, j) - closed.begin(); 
      double parity = nelecBefore % 2 == 0 ? 1. : -1.;

      if (type == -1) detTemp.setocc(j, false); // annihilated e from Det[k]
      else if (type == 1) detTemp.setocc(j, true); // added e to Det[k]
      int n = std::distance(DetsResponse, std::lower_bound(DetsResponse, DetsResponse+DetsResponseSize, detTemp)); // binary search for detTemp in DetsResponse
      ciResponse(n) = parity * ci[k];
    }
  }
  
  // preconditioner
  MatrixXx diag = MatrixXx::Zero(DetsResponseSize, 1);
  for (int k = 0; k < DetsResponseSize; k++) {
    if (type == -1) {double detEne = DetsResponse[k].Energy(I1, I2, coreE); diag(k, 0) = (detEne - E0 + conj(w)) * (detEne - E0 + w);} // annihilated e from Det[k]
    else if (type == 1) {double detEne = DetsResponse[k].Energy(I1, I2, coreE); diag(k, 0) = (detEne - E0 - conj(w)) * (detEne - E0 - w);} // added e to Det[k]
  }

#ifndef SERIAL
  world.barrier();
#endif
  
  // solving for x0 using eigen linear solver
  //MatrixXx x0 = MatrixXx::Zero(DetsNm1Size, 1);
  //MatrixXx HamiltonianNm1diag = HamiltonianNm1;// to store H0-E0-w
  //for(int k=0; k< DetsNm1Size; k++) HamiltonianNm1diag(k,k) = HamiltonianNm1(k,k)-E0-w;
  //x0 = HamiltonianNm1diag.colPivHouseholderQr().solve(ciNm1);

  // solve for x0 = 1/H0-E0+w a_j | psi0 > or x0 = 1/H0-E0-w a_j^(dag) | psi0 >
  // doing cg 
  MatrixXx x0 = MatrixXx::Zero(DetsResponseSize, 1);
  vector<CItype*> proj;
  if (type == -1) LinearSolver(HResponse, diag, E0-w, x0, ciResponse, proj, 1.e-5, false);
  else if (type == 1) LinearSolver(HResponse, diag, E0+w, x0, ciResponse, proj, 1.e-5, false);
  //if (type == -1) LinearSolver(HResponse, E0-w, x0, ciResponse, proj, 1.e-5, false);
  //else if (type == 1) LinearSolver(HResponse, E0+w, x0, ciResponse, proj, 1.e-5, false);
  
  //calc < psi0 | a_i^(dag) x0
  CItype overlap = 0.0;
  if (commrank == 0) {
    for (int k = 0; k < DetsSize; k++) {
      bool validDet = false;
      if (type == -1 && Dets[k].getocc(i)) validDet = true;
      else if (type == 1 && !Dets[k].getocc(i)) validDet = true;
      if (validDet) {
        detTemp = Dets[k];

        // parity : can be made faster if this becomes slow
        std::vector<int> closed(nelec);
        detTemp.getClosed(closed);
        int nelecBefore = std::lower_bound(closed.begin(), closed.begin() + nelec, i) - closed.begin(); 
        double parity = nelecBefore % 2 == 0 ? 1. : -1.;
        
        if (type == -1) detTemp.setocc(i, false); // annihilated e from Det[k]
        else if (type == 1) detTemp.setocc(i, true); // added e to Det[k]
        int n = std::distance(DetsResponse, std::lower_bound(DetsResponse, DetsResponse+DetsResponseSize, detTemp)); // binary search for detTemp in DetsResponse
        overlap += parity * ci[k] * x0(n);
      }
    }
  }
    
#ifndef SERIAL
  mpi::broadcast(world, overlap, 0);
#endif
    
  if (type == -1) return overlap;
  else if (type == 1) return -overlap;
}

// uses spectrum of the N-1 electron ham
// not parallelized 
CItype calcGreensFunctionExact(int i, int j, Determinant* Dets, CItype* ci, int DetsSize, 
        Determinant* DetsNm1v, int DetsNm1vSize, std::vector<MatrixXd>& ciNm1, std::vector<size_t>& idx, double E0, std::vector<double>& ENm1, CItype w) {

#ifndef SERIAL
  boost::mpi::communicator world;
#endif

  CItype green = 0.0;
  // calculating < psiNm1 | a_j | psi0 >
  if (commrank == 0) {
    // to store values in < psiNm1 | a_j | psi0 >, 
    MatrixXx summandValsj = MatrixXx::Zero(ciNm1.size(), 1);  
    Determinant detTemp;
    Determinant* pos; 
    for (int k = 0; k < DetsSize; k++){
      if (Dets[k].getocc(j)) {
        detTemp = Dets[k];
        detTemp.setocc(j, false); //annihilated Det[k]
        pos = std::lower_bound(DetsNm1v, DetsNm1v+DetsNm1vSize, detTemp);
        if (*pos == detTemp) {
          int n = std::distance(DetsNm1v, pos); 
          for (int l = 0; l < ciNm1.size(); l++) {
            summandValsj(l, 0) += ciNm1[l](idx[n])*ci[k];
          }
        }
      }
    }
 
    if (i == j) {
      for (int k = 0; k < summandValsj.size(); k++) green += summandValsj(k) * summandValsj(k) / (w-ENm1[k]+E0);
    } 
    else {
      MatrixXx summandValsi = MatrixXx::Zero(ciNm1.size(), 1);  
      // calculating < psiNm1 | a_i | psi0 >
      for (int k = 0; k < DetsSize; k++){
        if (Dets[k].getocc(i)) {
          detTemp = Dets[k];
          detTemp.setocc(i, false); //annihilated Det[k]
          pos = std::lower_bound(DetsNm1v, DetsNm1v+DetsNm1vSize, detTemp);
          if (*pos == detTemp) {
            int n = std::distance(DetsNm1v, pos); 
            for (int l = 0; l < ciNm1.size(); l++) summandValsi(l) += ciNm1[l](idx[n])*ci[k];
          }
        }
      }
      for (int k = 0; k < summandValsj.size(); k++) green += summandValsj(k) * summandValsi(k) / (w-ENm1[k]+E0);
    }
  }
    
#ifndef SERIAL
  mpi::broadcast(world, green, 0);
#endif
    
  return green;
}

/***
This does not work!
//to calculate G^1_ij(w) = <psi0| a_i* 1/(w-(H0-E0)) a_j 1/(H0-E0) H1 |psi0> + <psi0| H1 1/(H0-E0) a_i* 1/(w-(H0-E0)) a_j |psi0>
//w is a complex number
//psi0 is the variational ground state, a_i, a_j are annihilation operators 
//H0 is the Hamiltonian and E0 is the N electron variational ground state energy
//H1 is the perturbation
//Dets has dets in psi0, ci has the corresponding coeffs
//DetsNm1 dets in psi0 less one electron, HNm1 is H0 on the N-1 electron space
//DetsPert has dets in 1/(H0-E0) H1 |psi0>, ciPert has corresponding coeffs (each proc has a copy of these for now)
CItype calcFirstGreensFunction(int i, int j, Determinant* Dets, CItype* ci, int DetsSize, 
        Determinant* DetsNm1, int DetsNm1Size, Hmult2& HNm1, std::vector<Determinant>& DetsPert, std::vector<CItype>& ciPert, double E0, CItype w) {

#ifndef SERIAL
    boost::mpi::communicator world;
#endif

    //first evaluating 1/(w-(H0-E0)) a_i |psi0> (not w*)
    //to store ci coeffs in a_i|psi0>, corresponding to dets in DetsNm1
    MatrixXx ciNm1 = MatrixXx::Zero(DetsNm1Size, 1);  
    
    //calc a_i|psi0>
    //doing this on all procs
    Determinant detTemp;
    for(int k=0; k<DetsSize; k++){
        if(Dets[k].getocc(i)) {
            detTemp = Dets[k];
            detTemp.setocc(i, false); //annihilated Det[k]
            int n = std::distance(DetsNm1, std::lower_bound(DetsNm1, DetsNm1+DetsNm1Size, detTemp)); //binary search for detTemp in DetsNm1
            ciNm1(n) = ci[k];
        }
    }
    
#ifndef SERIAL
    world.barrier();
#endif
   
    //solving for x0 using eigen linear solver
    //MatrixXx x0 = MatrixXx::Zero(DetsNm1Size, 1);
    //MatrixXx HamiltonianNm1diag = HamiltonianNm1;// to store H0-E0-w
    //for(int k=0; k< DetsNm1Size; k++) HamiltonianNm1diag(k,k) = HamiltonianNm1(k,k)-E0-w;
    //x0 = HamiltonianNm1diag.colPivHouseholderQr().solve(ciNm1);

    //solve for x0 = 1/H0-E0-w a_i |psi0>
    MatrixXx x0 = MatrixXx::Zero(DetsNm1Size, 1);
    vector<CItype*> proj;
    LinearSolver(HNm1, E0+w, x0, ciNm1, proj, 1.e-5, false);
    

    //calculate overlap < x0 | a_j | ciPert >
    CItype overlap = 0.0;
    if(commrank == 0){
        for(int k=0; k<DetsPert.size(); k++){
            if(DetsPert[k].getocc(j)) {
                detTemp = DetsPert[k];
                detTemp.setocc(j, false); //annihilated DetPert[k]
                if(std::binary_search(DetsNm1, DetsNm1+DetsNm1Size, detTemp)){//binary search for detTemp in DetsNm1
                        int n = std::distance(DetsNm1, std::lower_bound(DetsNm1, DetsNm1+DetsNm1Size, detTemp)); 
                        overlap += ciPert[k]*x0(n);
                }
            }
        }
    }
    
#ifndef SERIAL
    mpi::broadcast(world, overlap, 0);
#endif

    return -2*overlap;

}
*/
