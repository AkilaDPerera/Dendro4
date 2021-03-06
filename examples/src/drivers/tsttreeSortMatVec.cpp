//
// Created by milinda on 6/23/16.
// @author: Milinda Shayamal Fernando
// School of Computing, University of Utah
//
// Example (similar version of the tstMatvec but using the treeSort functionality implemented in sfcSort.h)
//

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
#include "sfcSort.h"
#include "testUtils.h"
#include "octreeStatistics.h"


double**** LaplacianType2Stencil;
double**** MassType2Stencil;

#ifdef PETSC_USE_LOG
//user-defined variables
int Jac1DiagEvent;
int Jac1MultEvent;
int Jac1FinestDiagEvent;
int Jac1FinestMultEvent;
#endif

char sendComMapFileName[256];
char recvComMapFileName[256];


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




int main(int argc, char ** argv )
{



    int npes, rank;
    bool incCorner = 1;

    // Default values.
    unsigned int solveU = 0;
    unsigned int writeB = 0;
    unsigned int numLoops = 100;
    double tol=0.1;

    MPI_Comm globalComm=MPI_COMM_WORLD;

    char Kstr[20];
    char pFile[256],bFile[256],uFile[256];

    double gSize[3];
    unsigned int ptsLen;
    DendroIntL grainSize =10000;
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

    PetscInitialize(&argc,&argv,"options",NULL);

    ot::RegisterEvents();
    ot::DA_Initialize(globalComm);
    PetscErrorPrintf = PetscErrorPrintfNone;

    MPI_Comm_size(globalComm,&npes);
    MPI_Comm_rank(globalComm,&rank);


    if(argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << "inpfile grainSz[10000] dim[3] maxDepth[30] tol[0-1] genPts[1] solveU[0] writeB[0] maxNumPtsPerOctant[1] incCorner[1] numLoops[100] compressLut[1]" << std::endl;
        return -1;
    }

    if(argc > 2) {
        grainSize = atoi(argv[2]);
    }
    if(argc > 3) {
        dim = atoi(argv[3]);
    }
    if(argc > 4) {
        maxDepth = atoi(argv[4]);
    }
    if(argc > 5) {
        tol = atof(argv[5]);
    }
    if(argc > 6) {
        genPts = (bool)atoi(argv[6]);
    }
    if(argc > 7) {
        solveU = atoi(argv[7]);
    }
    if(argc > 8) {
        writeB = atoi(argv[8]);
    }
    if(argc > 9) {
        maxNumPts = atoi(argv[9]);
    }if(argc > 10) {
        incCorner = atoi(argv[10]);
    }if(argc > 11) {
        numLoops = atoi(argv[11]);
    }if(argc > 12) {
        compressLut = atoi(argv[12]);
    }

    _InitializeHcurve(dim);

#ifdef POWER_MEASUREMENT_TIMESTEP
    time_t rawtime;
    struct tm * ptm;
#endif


    if (!rank) {
        std::cout << BLU << "===============================================" << NRM << std::endl;
        std::cout << " Input Parameters" << std::endl;
        std::cout << " Input File Prefix:" << argv[1] << std::endl;
        std::cout << " Gen Pts files: " << genPts << std::endl;
        std::cout << " Number of Points per process: " << grainSize << std::endl;
        std::cout << " Dim: "<<dim<<std::endl;
        std::cout << " Max Depth: " << maxDepth << std::endl;
        std::cout << " Tol: "<<tol<<std::endl;
        std::cout << " MatVec number of iterations: "<<numLoops<<std::endl;
        std::cout << BLU << "===============================================" << NRM << std::endl;


    }


#ifdef HILBERT_ORDERING
    sprintf(sendComMapFileName,"sendCommMap_H_tol_%f_npes_%d_pts_%d_ps%d_%d.csv",tol,npes,grainSize,rank,npes);
    sprintf(recvComMapFileName,"recvCommMap_H_tol_%f_npes_%d_pts_%d_ps%d_%d.csv",tol,npes,grainSize,rank,npes);

#else
    sprintf(sendComMapFileName,"sendCommMap_M_tol_%f_npes_%d_pts_%d_ps%d_%d.csv",tol,npes,grainSize,rank,npes);
    sprintf(recvComMapFileName,"recvCommMap_M_tol_%f_npes_%d_pts_%d_ps%d_%d.csv",tol,npes,grainSize,rank,npes);

#endif

    ot::TreeNode root=ot::TreeNode(m_uiDim,maxDepth);

    if(genPts==1)
    {
        genGauss(0.15,grainSize,dim,argv[1],globalComm);
    }

    //genGauss(0.15,grainSize,dim,pts);

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


    ptsLen=pts.size();
    std::vector<ot::TreeNode> tmpNodes;
    DendroIntL totPts=grainSize*dim;

    pts2Octants(tmpNodes,&(*(pts.begin())),totPts,dim,maxDepth);
    pts.clear();
    std::vector<ot::TreeNode> tmpSorted;
    SFC::parSort::SFC_treeSort(tmpNodes,tmpSorted,tmpSorted,tmpSorted,tol,maxDepth,root,ROOT_ROTATION,1,TS_REMOVE_DUPLICATES,NUM_NPES_THRESHOLD,globalComm);
    std::swap(tmpNodes,tmpSorted);
    tmpSorted.clear();



#ifdef POWER_MEASUREMENT_TIMESTEP
    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" points2Octree Begin: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif

    startTime = MPI_Wtime();
    SFC::parSort::SFC_treeSort(tmpNodes,linOct,linOct,linOct,tol,maxDepth,root,ROOT_ROTATION,1,TS_CONSTRUCT_OCTREE,NUM_NPES_THRESHOLD,globalComm);
    tmpNodes.clear();
    endTime = MPI_Wtime();
#ifdef POWER_MEASUREMENT_TIMESTEP
    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" points2Octree End: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif

    //SFC::parSort::SFC_3D_Sort(linOct,tol,maxDepth,globalComm);


    localTime = endTime - startTime;
    par::Mpi_Reduce<double>(&localTime, &totalTime, 1, MPI_MAX, 0, MPI_COMM_WORLD);
    if (!rank) {
        std::cout << "P2n Time: " << totalTime << std::endl;
    }
    // reduce and only print the total ...
    locSz = linOct.size();
    par::Mpi_Reduce<DendroIntL>(&locSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);
    if (rank == 0) {
        std::cout << "# of Unbalanced Octants: " << totalSz << std::endl;
    }
    pts.clear();

#ifdef POWER_MEASUREMENT_TIMESTEP
    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" balOCt Begin: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif

    startTime = MPI_Wtime();
    SFC::parSort::SFC_treeSort(linOct,balOct,balOct,balOct,tol,maxDepth,root,ROOT_ROTATION,1,TS_BALANCE_OCTREE,NUM_NPES_THRESHOLD,globalComm);
    linOct.clear();
    //ot::balanceOctree(linOct, balOct, dim, maxDepth, incCorner, MPI_COMM_WORLD, NULL, NULL);
    endTime = MPI_Wtime();
#ifdef POWER_MEASUREMENT_TIMESTEP
    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" balOCt End: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif

    //treeNodesTovtk(balOct,rank,"balOct");

    locSz = balOct.size();
    localTime = endTime - startTime;
    par::Mpi_Reduce<DendroIntL>(&locSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);
    par::Mpi_Reduce<double>(&localTime, &totalTime, 1, MPI_MAX, 0, MPI_COMM_WORLD);

    DendroIntL balOctSz_g=0;

    if (!rank) {
        balOctSz_g=totalSz;
        std::cout << "# of Balanced Octants: " << totalSz << std::endl;
        std::cout << "bal Time: " << totalTime << std::endl;
    }

    //treeNodesTovtk(balOct,rank,"balOCt_dendro");

#ifdef POWER_MEASUREMENT_TIMESTEP
    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" oda Begin: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif


    // octree statistics computation.
    locSz = balOct.size();
    DendroIntL minSz,maxSz;
    double ld_imb=0;

    par::Mpi_Reduce<DendroIntL>(&locSz, &maxSz, 1, MPI_MAX, 0, MPI_COMM_WORLD);
    par::Mpi_Reduce<DendroIntL>(&locSz, &minSz, 1, MPI_MIN, 0, MPI_COMM_WORLD);

    ld_imb=maxSz/(double)minSz;
    double bdy_surfaces[3];
    calculateBoundaryFaces(balOct,1,bdy_surfaces,MPI_COMM_WORLD);

    if(!rank)
    {
        std::cout<<"=================================="<<std::endl;
        std::cout<<"        OCTREE STAT"<<std::endl;
        std::cout<<"=================================="<<std::endl;
        std::cout<<"npes\ttol\tlocalSz_min\tlocalSz_max\tld_imb\tbdy_min\tbdy_mean\tbdy_max"<<std::endl;
        std::cout<<npes<<"\t"<<tol<<"\t"<<minSz<<"\t"<<maxSz<<"\t"<<ld_imb<<"\t"<<bdy_surfaces[0]<<"\t"<<bdy_surfaces[2]<<"\t"<<bdy_surfaces[1]<<std::endl;

    }






    startTime = MPI_Wtime();
    ot::DA da(balOct, MPI_COMM_WORLD, MPI_COMM_WORLD,tol,compressLut);
    endTime = MPI_Wtime();

#ifdef POWER_MEASUREMENT_TIMESTEP
    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" oda End: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif


    balOct.clear();
    // compute total inp size and output size
    locSz = da.getNodeSize();
    localTime = endTime - startTime;
    par::Mpi_Reduce<DendroIntL>(&locSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);
    par::Mpi_Reduce<double>(&localTime, &totalTime, 1, MPI_MAX, 0, MPI_COMM_WORLD);

    DendroIntL vertexSz=0;

    if(!rank) {
        vertexSz=totalSz;
        std::cout << "Total # Vertices: "<< totalSz << std::endl;
        std::cout << "Time to build ODA: "<<totalTime << std::endl;
    }

#ifdef HILBERT_ORDERING
    da.computeHilbertRotations();
    if(!rank)
        std::cout<<"ODA Rotation pattern computation completed for Hilbert. "<<std::endl;
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


#ifdef POWER_MEASUREMENT_TIMESTEP

    time ( &rawtime );
    //std::this_thread::sleep_for(std::chrono::milliseconds(10000));
    ptm = gmtime ( &rawtime );
    if(!rank) std::cout<<" MatVec Begin: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
#endif

    double  numLocalNodes=da.getInternalNodeSize();
    double numGhostNodes=da.getPreAndPostGhostNodeSize();

    double numLocalNodes_g[3];
    double numGhostNodes_g[3];

    par::Mpi_Reduce(&numLocalNodes,numLocalNodes_g,1,MPI_MIN,0,globalComm);
    par::Mpi_Reduce(&numLocalNodes,numLocalNodes_g+1,1,MPI_SUM,0,globalComm);
    par::Mpi_Reduce(&numLocalNodes,numLocalNodes_g+2,1,MPI_MAX,0,globalComm);

    par::Mpi_Reduce(&numGhostNodes,numGhostNodes_g,1,MPI_MIN,0,globalComm);
    par::Mpi_Reduce(&numGhostNodes,numGhostNodes_g+1,1,MPI_SUM,0,globalComm);
    par::Mpi_Reduce(&numGhostNodes,numGhostNodes_g+2,1,MPI_MAX,0,globalComm);

    numLocalNodes_g[1]/=npes;
    numGhostNodes_g[1]/=npes;

    if(!rank)
    {
        std::cout<<" local (min, mean , max): "<<numLocalNodes_g[0]<<" , "<<numLocalNodes_g[1]<<" , "<<numLocalNodes_g[2]<<"\n ghost (min, mean , max): "<<numGhostNodes_g[0]<<" , "<<numGhostNodes_g[1]<<" , "<<numGhostNodes_g[2]<<std::endl;
    }

 for(unsigned int i=0;i<5;i++) { // warmup
        iC(Jacobian1MatGetDiagonal(J, diag));
        iC(Jacobian1MatMult(J, in, out));
 }

auto mVecBegin=std::chrono::high_resolution_clock::now();
    for(unsigned int i=0;i<numLoops;i++) {
        iC(Jacobian1MatGetDiagonal(J, diag));
        iC(Jacobian1MatMult(J, in, out));
    }
auto mVecEnd=std::chrono::high_resolution_clock::now();
double mVecTime=std::chrono::duration_cast<std::chrono::milliseconds>(mVecEnd - mVecBegin).count();

    double mvecTime_g[3]; // min mean max
    par::Mpi_Reduce(&mVecTime,&mvecTime_g[0],1,MPI_MIN,0,globalComm);
    par::Mpi_Reduce(&mVecTime,&mvecTime_g[1],1,MPI_SUM,0,globalComm);
    par::Mpi_Reduce(&mVecTime,&mvecTime_g[2],1,MPI_MAX,0,globalComm);

    mvecTime_g[1]=mvecTime_g[1]/(double)npes;

    if(!rank)
    {
        std::cout<<"npes\tgrainSz\ttol\tbalOctSz\tvertexSz\tnumLoops\tmVecMin\tmVecMean\tmVecMax"<<std::endl;
        std::cout<<npes<<"\t"<<grainSize<<"\t"<<tol<<"\t"<<balOctSz_g<<"\t"<<vertexSz<<"\t"<<numLoops<<"\t"<<mvecTime_g[0]<<"\t"<<mvecTime_g[1]<<"\t"<<mvecTime_g[2]<<std::endl;
    }




#ifdef POWER_MEASUREMENT_TIMESTEP
  time ( &rawtime );
  ptm = gmtime ( &rawtime );
  if(!rank) std::cout<<" MatVec End: "<<(ptm->tm_year+1900)<<"-"<<(ptm->tm_mon+1)<<"-"<<ptm->tm_mday<<" "<<(ptm->tm_hour%24)<<":"<<ptm->tm_min<<":"<<ptm->tm_sec<<std::endl;
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


}