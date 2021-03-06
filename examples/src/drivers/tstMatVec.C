//#include "papi.h"
//#include <papiStdEventDefs.h>


#include "mpi.h"
#include "petsc.h"
#include "sys.h"
#include <vector>
#include "TreeNode.h"
#include "parUtils.h"
#include "oda.h"
#include "handleStencils.h"
#include "odaJac.h"
#include "colors.h"
#include <cstdlib>
#include <cstring>
#include "externVars.h"
#include "dendro.h"
#include <iostream>
#include <string>
#include <stdio.h>
#include <time.h>
#include <time.h>

#include "genPts_par.h"
#include <climits>
#include <chrono>
#include <thread>


//Don't want time to be synchronized. Need to check load imbalance.
#ifdef MPI_WTIME_IS_GLOBAL
#undef MPI_WTIME_IS_GLOBAL
#endif



#ifdef __PAPI_PROFILING__
#include <papi.h>
#endif




#ifdef PETSC_USE_LOG
//user-defined variables
int Jac1DiagEvent;
int Jac1MultEvent;
int Jac1FinestDiagEvent;
int Jac1FinestMultEvent;
#endif

double**** LaplacianType2Stencil; 
double**** MassType2Stencil;


const std::string currentDateTime() {
  time_t     now = time(0);
  struct tm  tstruct;
  char       buf[80];
  tstruct = *localtime(&now);
  // Visit http://en.cppreference.com/w/cpp/chrono/c/strftime
  // for more information about date/time format
  strftime(buf, sizeof(buf), "%Y-%m-%d.%X", &tstruct);

  return buf;
}





int main(int argc, char ** argv ) {	
  int npes, rank;
  bool incCorner = 1;  
  unsigned int numPts;
  unsigned int solveU = 0;
  unsigned int writeB = 0;
  unsigned int numLoops = 1;
  char Kstr[20];
  char pFile[256],bFile[256],uFile[256];
  double gSize[3];
  unsigned int ptsLen;
  unsigned int maxNumPts= 1;
  unsigned int dim=3;
  unsigned int maxDepth=30;
  bool compressLut=true;
  bool genPts=true;
  bool genRegGrid=false;
  double localTime, totalTime;
  double startTime, endTime;
  DendroIntL locSz, totalSz;
  std::vector<ot::TreeNode> linOct, balOct;
  std::vector<double> pts;
  DendroIntL grainSize =10000;
  char nlistFName[256];

  PetscInitialize(&argc,&argv,"options",NULL);
  ot::RegisterEvents();
  ot::DA_Initialize(MPI_COMM_WORLD);
  PetscErrorPrintf = PetscErrorPrintfNone;
  unsigned int regLev;





#ifdef PETSC_USE_LOG
  PetscClassId classid;
  PetscClassIdRegister("Dendro",&classid);

  PetscLogEventRegister("ODAmatDiag",classid, &Jac1DiagEvent);
  PetscLogEventRegister("ODAmatMult",classid, &Jac1MultEvent);
  PetscLogEventRegister("ODAmatDiagFinest",classid, &Jac1FinestDiagEvent);
  PetscLogEventRegister("ODAmatMultFinest",classid, &Jac1FinestMultEvent);
  int stages[4];
  PetscLogStageRegister("P2O.",&stages[0]);
  PetscLogStageRegister("Bal",&stages[1]);
  PetscLogStageRegister("ODACreate",&stages[2]);
  PetscLogStageRegister("MatVec",&stages[3]);
#endif



  MPI_Comm_size(MPI_COMM_WORLD,&npes);
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  if(argc < 6) {
    std::cerr << "Usage: " << argv[0] << " inpfile totalPts[10000] dim[3] maxDepth[30] tol[0-1] genPts[1] solveU[0] writeB[0] maxNumPtsPerOctant[1] incCorner[1] numLoops[100] compressLut[1]" << std::endl;
    return -1;
  }

  grainSize=atoi(argv[2]);
  dim = atoi(argv[3]);
  maxDepth = atoi(argv[4]);
  double tol=atof(argv[5]);
  genPts = (bool)(atoi(argv[6]));

  if(argc > 7) { solveU = atoi(argv[7]);}
  if(argc > 8) {  writeB = atoi(argv[8]);; }
  if(argc > 9) { maxNumPts = atoi(argv[9]);}
  if(argc > 10) { incCorner=(bool)(atoi(argv[10]));}
  if(argc >11 ) { numLoops=atoi(argv[11]);}
  if(argc >12) {compressLut=(bool)(atoi(argv[12]));}


  _InitializeHcurve(dim);



    double t_rd=0;
    double t_cons=0;
    double t_bal=0;
    double t_mesh=0;


    double t_rd_g[3];
    double t_cons_g[3];
    double t_bal_g[3]; //0 -min 1-mean 2-Max
    double t_mesh_g[3]; //0 -min 1-mean 2-Max

    DendroIntL numUniqueOct;
    DendroIntL numConsOct;
    DendroIntL numBalOct;
    DendroIntL localSz;
    DendroIntL globalSz;

    MPI_Comm comm =MPI_COMM_WORLD;


    if (!rank) {
    std::cout << BLU << "===============================================" << NRM << std::endl;
    std::cout << " Input Parameters" << std::endl;
    std::cout << " Input File Prefix:" << argv[1] << std::endl;
    std::cout << " Gen Pts files:: " << genPts << std::endl;
    std::cout << " Number of Points per process:: " << grainSize << std::endl;
    std::cout << " Max Depth:" << maxDepth << std::endl;
    std::cout << " Gen Regular Grid:"<<genRegGrid<<std::endl;
    std::cout << " Regular grid Level:"<<regLev<<std::endl;
    std::cout << " Tol: "<<tol<<std::endl;
    std::cout << BLU << "===============================================" << NRM << std::endl;
  }


    if(genPts) {

       genGauss(0.15,grainSize,dim,argv[1],comm);

    }

    sprintf(pFile, "%s%d_%d.pts", argv[1], rank, npes);
    //std::cout<<"Attempt to Read "<<ptsFileName<<std::endl;

    //Read pts from files
    if (!rank) {
        std::cout << RED " Reading  " << argv[1] << NRM << std::endl; // Point size
    }
    ot::readPtsFromFile(pFile, pts);

    if (!rank) {
        std::cout << GRN " Finished reading  " << argv[1] << NRM << std::endl; // Point size
    }


  //std::cout<<"pts npes : "<<pts.size()<<std::endl;
  std::vector<ot::TreeNode> tmpNodes;
  for (int i = 0; i < pts.size(); i += 3) {
    if ((pts[i] > 0.0) &&
        (pts[i + 1] > 0.0)
        && (pts[i + 2] > 0.0) &&
        (((unsigned int) (pts[i] * ((double) (1u << maxDepth)))) < (1u << maxDepth)) &&
        (((unsigned int) (pts[i + 1] * ((double) (1u << maxDepth)))) < (1u << maxDepth)) &&
        (((unsigned int) (pts[i + 2] * ((double) (1u << maxDepth)))) < (1u << maxDepth))) {
#ifdef __DEBUG__
      assert((i+2) < ptsLen);
#endif
      tmpNodes.push_back(ot::TreeNode((unsigned int) (pts[i] * (double) (1u << maxDepth)),
                                      (unsigned int) (pts[i + 1] * (double) (1u << maxDepth)),
                                      (unsigned int) (pts[i + 2] * (double) (1u << maxDepth)),
                                      maxDepth, dim, maxDepth));
    }
  }
  pts.clear();
//  if(!rank) {
//    std::cout << "Number of Nodes Read:" << tmpNodes.npes() << std::endl;
//    for(int i=0;i<tmpNodes.npes();i++)
//      std::cout<<"Node:"<<tmpNodes[i]<<std::endl;
//  }

  startTime=MPI_Wtime();
  par::removeDuplicates<ot::TreeNode>(tmpNodes, false, MPI_COMM_WORLD);
  endTime=MPI_Wtime();
  t_rd = endTime - startTime;

  par::Mpi_Reduce(&t_rd, t_rd_g, 1, MPI_MIN, 0, comm);
  par::Mpi_Reduce(&t_rd, t_rd_g + 1, 1, MPI_SUM, 0, comm);
  par::Mpi_Reduce(&t_rd, t_rd_g + 2, 1, MPI_MAX, 0, comm);
  t_rd_g[1] = t_rd_g[1] / npes;

  localSz=tmpNodes.size();
  par::Mpi_Reduce(&localSz,&globalSz,1,MPI_SUM,0,comm);
  numUniqueOct=globalSz;

    if(!rank)
    {
        std::cout << YLW<< "Number of unique octants: "<<numUniqueOct<<NRM<<std::endl;
        std::cout << YLW<< "Remove duplicates time(max): "<<t_rd_g[2]<<NRM<<std::endl;
    }

  linOct = tmpNodes;
  tmpNodes.clear();

  par::partitionW<ot::TreeNode>(linOct, NULL, MPI_COMM_WORLD);
  // reduce and only print the total ...
  /*locSz = linOct.size();
  par::Mpi_Reduce<DendroIntL>(&locSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);
  if (rank == 0) {
    std::cout << " # pts= " << totalSz << std::endl;
  }*/

   //std::cout << rank<<" # pts= " << totalSz << std::endl;
  //std::cout<<"linOct:"<<linOct.npes()<<std::endl;
  pts.resize(3 * (linOct.size()));
  ptsLen = (3 * (linOct.size()));
  for (int i = 0; i < linOct.size(); i++) {
    pts[3 * i] = (((double) (linOct[i].getX())) + 0.5) / ((double) (1u << maxDepth));
    pts[(3 * i) + 1] = (((double) (linOct[i].getY())) + 0.5) / ((double) (1u << maxDepth));
    pts[(3 * i) + 2] = (((double) (linOct[i].getZ())) + 0.5) / ((double) (1u << maxDepth));
  }//end for i
  linOct.clear();
  gSize[0] = 1.;
  gSize[1] = 1.;
  gSize[2] = 1.;

  //std::cout << rank << " : reached barrier" << std::endl;
  MPI_Barrier(MPI_COMM_WORLD);
  //std::cout << rank << " : reached barrier" << std::endl;
#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[0]);
#endif
  startTime = MPI_Wtime();
  ot::points2Octree(pts, gSize, linOct, dim, maxDepth, maxNumPts, MPI_COMM_WORLD);
  endTime = MPI_Wtime();
#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif
  t_cons = endTime - startTime;
  par::Mpi_Reduce(&t_cons, t_cons_g, 1, MPI_MIN, 0, comm);
  par::Mpi_Reduce(&t_cons, t_cons_g + 1, 1, MPI_SUM, 0, comm);
  par::Mpi_Reduce(&t_cons, t_cons_g + 2, 1, MPI_MAX, 0, comm);
  t_cons_g[1] = t_cons_g[1] / npes;


  localSz=linOct.size();
  par::Mpi_Reduce(&localSz,&globalSz,1,MPI_SUM,0,comm);
  numConsOct=globalSz;
  if(!rank)
  {
      std::cout << YLW<< "Number of unbalanced octants: "<<numConsOct<<NRM<<std::endl;
      std::cout << YLW<< "Tree construction time(max): "<<t_cons_g[2]<<NRM<<std::endl;
  }
  // reduce and only print the total ...

  pts.clear();

  //Balancing...
  MPI_Barrier(MPI_COMM_WORLD);
#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[1]);
#endif
  startTime = MPI_Wtime();
  ot::balanceOctree(linOct, balOct, dim, maxDepth, incCorner, MPI_COMM_WORLD, NULL, NULL);
  endTime = MPI_Wtime();



#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif
  linOct.clear();
  if (writeB) {
    ot::writeNodesToFile(bFile, balOct);
  }
  // compute total inp npes and output npes
  t_bal= endTime - startTime;


    par::Mpi_Reduce(&t_bal, t_bal_g, 1, MPI_MIN, 0, comm);
    par::Mpi_Reduce(&t_bal, t_bal_g + 1, 1, MPI_SUM, 0, comm);
    par::Mpi_Reduce(&t_bal, t_bal_g + 2, 1, MPI_MAX, 0, comm);
    t_bal_g[1] = t_bal_g[1] / npes;

    localSz=balOct.size();
    par::Mpi_Reduce(&localSz,&globalSz,1,MPI_SUM,0,comm);
    numBalOct=globalSz;

    if(!rank) {
        std::cout << YLW<<"Number of balanced octants: " << numBalOct <<NRM<< std::endl;
        std::cout<< YLW<<" Tree balancing time (max): "<<t_bal_g[2]<<NRM<<std::endl;
    }

  //ODA ...
  MPI_Barrier(MPI_COMM_WORLD);
#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[2]);
#endif
  assert(!(balOct.empty()));
  startTime = MPI_Wtime();
  ot::DA da(balOct, MPI_COMM_WORLD, MPI_COMM_WORLD, compressLut);
  endTime = MPI_Wtime();
#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif
  balOct.clear();
  // compute total inp npes and output npes
  locSz = da.getNodeSize();
  t_mesh = endTime - startTime;
  par::Mpi_Reduce<DendroIntL>(&locSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);

  par::Mpi_Reduce(&t_mesh, t_mesh_g, 1, MPI_MIN, 0, comm);
  par::Mpi_Reduce(&t_mesh, t_mesh_g + 1, 1, MPI_SUM, 0, comm);
  par::Mpi_Reduce(&t_mesh, t_mesh_g + 2, 1, MPI_MAX, 0, comm);
  t_mesh_g[1] = t_mesh_g[1] / npes;

  if(!rank) {
      std::cout<< YLW<<"Mesh generation time (max): "<<t_mesh_g[2]<<NRM<<std::endl;
      std::cout << "Total # Vertices: "<< totalSz << std::endl;
  }
 dollar::text(std::cout);

#ifdef HILBERT_ORDERING
  da.computeHilbertRotations();
  if(!rank)
    std::cout<<"ODA Rotation pattern computation completed for Hilbert. "<<std::endl;
#endif


  //////////////////////////////////////////////////////////////////////////////////////////////////////ODA STATISTICS////////////////////////////////////////////////////////////////////////////////////////////////////////////
  //! Quality of the partition ...

  da.printODAStatistics();
  da.printODANodeListStatistics(nlistFName);

  //////////////////////////////////////////////////////////////////////////////////////////////////////ODA STATISTICS////////////////////////////////////////////////////////////////////////////////////////////////////////////

 /*
  * This Code is to Visually check the build node list correctness by writing the nodes to vtk file.
  *
  * unsigned int nodeList[8];
  unsigned int nIndex=0.4*(da.end<ot::DA_FLAGS::ALL>());// (unsigned int )0.3*da.end<ot::DA_FLAGS::ALL>();
  std::vector<ot::TreeNode> node;
  ot::TreeNode tmp;

  bool state=false;

  for(da.init<ot::DA_FLAGS::ALL>();da.curr()< da.end<ot::DA_FLAGS::ALL>();da.next<ot::DA_FLAGS::ALL>())
  {
    da.getNodeIndices(nodeList);
    if(da.curr()<da.getIdxElementBegin())
    {
      state=true;
      Point p=da.getCurrentOffset();
      tmp=ot::TreeNode(1,p.xint(),p.yint(),p.zint(),da.getLevel(da.curr()),3,da.getMaxDepth());
      node.push_back(tmp);
      da.getNodeIndices(nodeList);
      treeNodesTovtk(node,rank,"node_pre");
      break;
    }

  }

  std::vector<ot::TreeNode> keys;
  unsigned int curentIndex;
 if(state) {
   for (da.init<ot::DA_FLAGS::ALL>(); da.curr() < da.end<ot::DA_FLAGS::ALL>(); da.next<ot::DA_FLAGS::ALL>()) {
     curentIndex = da.curr();

     for (int w = 0; w < 8; w++) {
       if (curentIndex == nodeList[w]) {
         Point p = da.getCurrentOffset();
         tmp = ot::TreeNode(1, p.xint(), p.yint(), p.zint(), da.getLevel(curentIndex), 3, da.getMaxDepth());
         keys.push_back(tmp);
       }
     }
   }
   treeNodesTovtk(keys, rank, "keys_pre");
 }

  node.clear();
  keys.clear();
*/
  MPI_Barrier(MPI_COMM_WORLD);
#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[3]);
#endif

  Mat J;
  Vec in, out, diag;
  PetscScalar zero = 0.0;

  //Nodal, Non-Ghosted
  da.createVector(in,false,false,1);
  da.createVector(out,false,false,1);
  da.createVector(diag,false,false,1);

  createLmatType2(LaplacianType2Stencil);
  createMmatType2(MassType2Stencil);
  if(!rank) {
    std::cout << "Created stencils."<< std::endl;
  }

  if(!rank) {
    std::cout<<rank << " Creating Jacobian" << std::endl;
  }

  iC(CreateJacobian1(&da,&J));

  if(!rank) {
    std::cout<<rank << " Computing Jacobian" << std::endl;
  }

  iC(ComputeJacobian1(&da,J));

  if(!rank) {
    std::cout<<rank << " Finished computing Jacobian" << std::endl;
  }

  VecSet(in, zero);

#ifdef __PAPI_PROFILING__
DendroIntL papi_counters []={0,0,0};
int papi_num_events=2;
int papi_retval=0;
int papi_events[]={PAPI_L1_TCM,PAPI_L2_DCM,PAPI_L2_DCH};

  papi_retval=PAPI_library_init(PAPI_VER_CURRENT);
  if(papi_retval!=PAPI_VER_CURRENT)
  {
    std::cout<<"Papi Initialization failed"<<std::endl;
  }

  papi_retval= PAPI_start_counters(papi_events,papi_num_events);
  if(papi_retval!=PAPI_OK)
  {
    std::cout<<"Counter initialization failed:"<<papi_retval<<std::endl;
  }

#endif

#ifdef POWER_MEASUREMENT_TIMESTEP

  time_t rawtime;
  struct tm * ptm;

  time ( &rawtime );


std::this_thread::sleep_for(std::chrono::milliseconds(60000));
 ptm = gmtime ( &rawtime );
 if(!rank) std::cout<<" MatVec Begin: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif

  for(unsigned int i=0;i<numLoops;i++) {
    iC(Jacobian1MatGetDiagonal(J, diag));
    iC(Jacobian1MatMult(J, in, out));
  }

#ifdef POWER_MEASUREMENT_TIMESTEP
  time ( &rawtime );
  ptm = gmtime ( &rawtime );
  if(!rank) std::cout<<" MatVec Begin: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif
#ifdef __PAPI_PROFILING__


  papi_retval=PAPI_read_counters(papi_counters,papi_num_events);
  papi_counters[0]=papi_counters[0]/numLoops;
  papi_counters[1]=papi_counters[1]/numLoops;
  papi_counters[2]=papi_counters[2]/numLoops;

  //std::cout<<"L1DataCacheMisses:"<<(papi_counters[0]-papi_counters[2])<<std::endl;
  std::cout<<"L1TotalCacheMisses:"<<papi_counters[0]<<std::endl;

  std::cout<<"L2DataCacheMisses:"<<papi_counters[1]<<std::endl;


#endif

  VecDestroy(&in);
  VecDestroy(&out);
  VecDestroy(&diag);

  iC(Jacobian1MatDestroy(J));

  destroyLmatType2(LaplacianType2Stencil);
  destroyMmatType2(MassType2Stencil);

  if(!rank) {
    std::cout << "Destroyed stencils."<< std::endl;
  }


#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif
  if (!rank) {
    std::cout << GRN << "Finalizing PETSC" << NRM << std::endl;
  }


#ifdef HILBERT_ORDERING
  delete [] rotations;
  rotations=NULL;
  delete [] HILBERT_TABLE;
  HILBERT_TABLE=NULL;
#endif


  ot::DA_Finalize();
  PetscFinalize();



}//end function

