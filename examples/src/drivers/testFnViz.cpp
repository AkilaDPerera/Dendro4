
#include "mpi.h"
#include "petsc.h"
#include "sys.h"
#include "parUtils.h"
#include "octUtils.h"
#include "TreeNode.h"
#include <cstdlib>
#include "externVars.h"
#include "dendro.h"

#ifdef MPI_WTIME_IS_GLOBAL
#undef MPI_WTIME_IS_GLOBAL
#endif

#ifndef iC
#define iC(fun) {CHKERRQ(fun);}
#endif

double gSize[3];

double gaussian(double mean, double std_deviation);

void setScalarByFunction(ot::DA* da, Vec vec, std::function<double(double,double,double)> f);

void saveNodalVecAsVTK(ot::DA* da, Vec vec, char *fname);

int main(int argc, char ** argv ) {	
  int size, rank;
  double startTime, endTime;
  double localTime, globalTime;
  unsigned int local_num_pts = 5000;
  unsigned int dim = 3;
  unsigned int maxDepth = 30;
  unsigned int maxNumPts = 1;
  bool writePts = false;
  bool incCorner = 1;  
  bool compressLut = false;
  
  std::vector<ot::TreeNode> linOct;
  std::vector<ot::TreeNode> balOct;
  std::vector<ot::TreeNode> tmpNodes;
  std::vector<double> pts;
  
  unsigned int ptsLen;
  DendroIntL localSz, totalSz;

  PetscInitialize(&argc, &argv, "options", NULL);
  ot::RegisterEvents();
  ot::DA_Initialize(MPI_COMM_WORLD);
  _InitializeHcurve(3);

#ifdef PETSC_USE_LOG
  PetscClassId classid;
  PetscClassIdRegister("Dendro",&classid);

  int stages[3];
  PetscLogStageRegister("P2O.",&stages[0]);
  PetscLogStageRegister("Bal",&stages[1]);
  PetscLogStageRegister("ODACreate",&stages[2]);
#endif



  MPI_Comm_size(MPI_COMM_WORLD, &size);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);

  std::cout << "Usage: <exe> local_num_pts[5000] dim[3] maxDepth[30] maxNumPtsPerOctant[1]  incCorner[1] compressLut[0] " << std::endl;

  if(argc > 1) {
    local_num_pts = atoi(argv[1]);
  }  
  if(argc > 2) {
    dim = atoi(argv[2]);
  }
  if(argc > 3) {
    maxDepth = atoi(argv[3]);
  }
  if(argc > 4) {
    maxNumPts = atoi(argv[4]);
  }
  if(argc > 5) {
    incCorner = (bool)(atoi(argv[5]));
  }  
  if(argc > 6) { 
    compressLut = (bool)(atoi(argv[6]));
  }

  MPI_Barrier(MPI_COMM_WORLD);
  if(!rank){
    std::cout << "Creating points on the fly... "<<std::endl;
  }

  startTime = MPI_Wtime();

  srand48(0x12345678 + 76543*rank);

  pts.resize(3*local_num_pts);
  for(unsigned int i = 0; i < (3*local_num_pts); i++) {
    pts[i]= gaussian(0.5, 0.16);
  }

  MPI_Barrier(MPI_COMM_WORLD);
  if(!rank){
    std::cout << " finished creating points  "<<std::endl; // Point size
  }
  endTime = MPI_Wtime();
  localTime = endTime - startTime;
  if(!rank){
    std::cout <<"point generation time: "<<localTime << std::endl;
  }

  ptsLen = pts.size();

  if(writePts) {
    char ptsFileName[100];
    sprintf(ptsFileName, "tempPts%d_%d.pts", rank, size);
    ot::writePtsToFile(ptsFileName, pts);
  }

  for(unsigned int i = 0; i < ptsLen; i+=3) {
    if( (pts[i] > 0.0) &&
        (pts[i+1] > 0.0)
        && (pts[i+2] > 0.0) &&
        ( ((unsigned int)(pts[i]*((double)(1u << maxDepth)))) < (1u << maxDepth))  &&
        ( ((unsigned int)(pts[i+1]*((double)(1u << maxDepth)))) < (1u << maxDepth))  &&
        ( ((unsigned int)(pts[i+2]*((double)(1u << maxDepth)))) < (1u << maxDepth)) ) {
      tmpNodes.push_back( ot::TreeNode((unsigned int)(pts[i]*(double)(1u << maxDepth)),
            (unsigned int)(pts[i+1]*(double)(1u << maxDepth)),
            (unsigned int)(pts[i+2]*(double)(1u << maxDepth)),
            maxDepth,dim,maxDepth) );
    }
  }
  pts.clear();

  MPI_Barrier(MPI_COMM_WORLD);

  if(!rank){
    std::cout << "num points: " <<  tmpNodes.size() << std::endl;
    std::cout <<"Removing bad points... "<<localTime << std::endl;
  }

  par::removeDuplicates<ot::TreeNode>(tmpNodes, false, MPI_COMM_WORLD);
  linOct = tmpNodes;
  tmpNodes.clear();

  MPI_Barrier(MPI_COMM_WORLD);

  if(!rank){
    std::cout <<"Partitioning Input... "<<localTime << std::endl;
  }

  par::partitionW<ot::TreeNode>(linOct, NULL, MPI_COMM_WORLD);

  // reduce and only print the total ...
  localSz = linOct.size();
  par::Mpi_Reduce<DendroIntL>(&localSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);

  MPI_Barrier(MPI_COMM_WORLD);

  if(!rank) {
    std::cout<<"# pts= " << totalSz<<std::endl;
  }

  MPI_Barrier(MPI_COMM_WORLD);

  pts.resize(3*(linOct.size()));
  ptsLen = (3*(linOct.size()));
  for(int i = 0; i < linOct.size(); i++) {
    pts[3*i] = (((double)(linOct[i].getX())) + 0.5)/((double)(1u << maxDepth));
    pts[(3*i)+1] = (((double)(linOct[i].getY())) +0.5)/((double)(1u << maxDepth));
    pts[(3*i)+2] = (((double)(linOct[i].getZ())) +0.5)/((double)(1u << maxDepth));
  }//end for i
  linOct.clear();

  gSize[0] = 1.0;
  gSize[1] = 1.0;
  gSize[2] = 1.0;

  MPI_Barrier(MPI_COMM_WORLD);	

#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[0]);
#endif
  startTime = MPI_Wtime();
  ot::points2Octree(pts, gSize, linOct, dim, maxDepth, maxNumPts, MPI_COMM_WORLD);
  endTime = MPI_Wtime();
  localTime = endTime - startTime;
#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif
  par::Mpi_Reduce<double>(&localTime, &globalTime, 1, MPI_MAX, 0, MPI_COMM_WORLD);
  if(!rank){
    std::cout <<"P2n Time: "<<globalTime << "secs " << std::endl;
  }
  pts.clear();

  localSz = linOct.size();
  par::Mpi_Reduce<DendroIntL>(&localSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);
  if(rank==0) {
    std::cout<<"linOct.size = " << totalSz<<std::endl;
  }

  MPI_Barrier(MPI_COMM_WORLD);	

#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[1]);
#endif
  startTime = MPI_Wtime();
  ot::balanceOctree (linOct, balOct, dim, maxDepth, incCorner, MPI_COMM_WORLD, NULL, NULL);
  endTime = MPI_Wtime();
  localTime = endTime - startTime;
#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif

  par::Mpi_Reduce<double>(&localTime, &globalTime, 1, MPI_MAX, 0, MPI_COMM_WORLD);
  if(!rank){
    std::cout <<"Bal Time: "<<globalTime << "secs " << std::endl;
  }
  linOct.clear();

  localSz = balOct.size();
  par::Mpi_Reduce<DendroIntL>(&localSz, &totalSz, 1, MPI_SUM, 0, MPI_COMM_WORLD);
  if(rank==0) {
    std::cout<<"balOct.size = " << totalSz<<std::endl;
  }

#ifdef PETSC_USE_LOG
  PetscLogStagePush(stages[2]);
#endif

  MPI_Barrier(MPI_COMM_WORLD);
  if(rank==0) {
    std::cout << "building DA" << std::endl;
  }

  ot::DA da(balOct, PETSC_COMM_WORLD, PETSC_COMM_WORLD, compressLut);
  
  if(rank==0) {
    std::cout << "finished building DA" << std::endl;
  }


#ifdef PETSC_USE_LOG
  PetscLogStagePop();
#endif

 //! Create nodal vector and set via function
 auto fx = [](double x, double y, double z) { return sin(2*M_PI*x)*sin(2*M_PI*y)*sin(2*M_PI*z); };

  //Nodal, non-Ghosted, dof
  PetscScalar zero = 0.0;
  Vec v;
  da.createVector(v, false, false, 1);

  // not really needed ...
  VecSet(v, zero);

  setScalarByFunction(&da, v, fx);

 //! write nodal vector to vtk file ...
  saveNodalVecAsVTK(&da, v, "fnViz" );


  // clean up 
  VecDestroy(&v);

  ot::DA_Finalize();
  PetscFinalize();
}//end main

double gaussian(double mean, double std_deviation) {
  static double t1 = 0, t2=0;
  double x1, x2, x3, r;

  using namespace std;

  // reuse previous calculations
  if(t1) {
    const double tmp = t1;
    t1 = 0;
    return mean + std_deviation * tmp;
  }
  if(t2) {
    const double tmp = t2;
    t2 = 0;
    return mean + std_deviation * tmp;
  }

  // pick randomly a point inside the unit disk
  do {
    x1 = 2 * drand48() - 1;
    x2 = 2 * drand48() - 1;
    x3 = 2 * drand48() - 1;
    r = x1 * x1 + x2 * x2 + x3*x3;
  } while(r >= 1);

  // Box-Muller transform
  r = sqrt(-2.0 * log(r) / r);

  // save for next call
  t1 = (r * x2);
  t2 = (r * x3);

  return mean + (std_deviation * r * x1);
}//end gaussian

void setScalarByFunction(ot::DA* da, Vec vec, std::function<double(double,double,double)> f) {
	int dof=1;	
  PetscScalar *_vec=NULL; 

  da->vecGetBuffer(vec,   _vec, false, false, true,  dof);
  
  da->ReadFromGhostsBegin<PetscScalar>(_vec, dof);
	da->ReadFromGhostsEnd<PetscScalar>(_vec);
		
  unsigned int maxD = da->getMaxDepth();
	unsigned int lev;
	double hx, hy, hz;
	Point pt;

	double xFac = gSize[0]/((double)(1<<(maxD-1)));
	double yFac = gSize[1]/((double)(1<<(maxD-1)));
	double zFac = gSize[2]/((double)(1<<(maxD-1)));
	double xx, yy, zz;
	unsigned int idx[8];

  for ( da->init<ot::DA_FLAGS::WRITABLE>(); da->curr() < da->end<ot::DA_FLAGS::WRITABLE>(); da->next<ot::DA_FLAGS::WRITABLE>() ) { 
    // set the value
    lev = da->getLevel(da->curr());
    hx = xFac*(1<<(maxD - lev));
    hy = yFac*(1<<(maxD - lev));
    hz = zFac*(1<<(maxD - lev));

    pt = da->getCurrentOffset();
			
    xx = pt.x()*xFac; yy = pt.y()*yFac; zz = pt.z()*zFac;
    
    da->getNodeIndices(idx);
    if ( ! da->isHanging( da->curr() ) ) {
      _vec[idx[0]] = f(xx, yy, zz);
     }
  }

  da->vecRestoreBuffer(vec,  _vec, false, false, true,  dof);
  
}

void saveNodalVecAsVTK(ot::DA* da, Vec vec, char *file_prefix) {
  int rank, size;
  char fname[256];

  
	MPI_Comm_rank(da->getComm(), &rank);
	MPI_Comm_size(da->getComm(), &size);
  
  sprintf(fname, "%s_%05d.vtk", file_prefix, rank);

  if ( !rank ) std::cout << "Writing to VTK file: " << fname << std::endl;

  std::ofstream out;
  out.open( fname );
  
  
  out << "# vtk DataFile Version 2.0" << std::endl;
  out << "DENDRO OCTREES" << std::endl;
  out << "ASCII" << std::endl;
  out << "DATASET UNSTRUCTURED_GRID" << std::endl;

  int dim = 3;

  int unit_points = 1 << dim;
  int num_verticies = da->getElementSize() * (unit_points);
  int num_cells = da->getElementSize();

  out << "POINTS " << num_verticies << " float" << std::endl;

  { // dim = 3
    
    unsigned int len; //!  ??
    unsigned int xl, yl, zl;  //! ??

    int num_data_field = 2; // rank and data 
    int num_cells_elements = num_cells * unit_points + num_cells;

    int dof=1;	
    PetscScalar *_vec=NULL; 

    da->vecGetBuffer(vec,   _vec, false, false, true,  dof);

    da->ReadFromGhostsBegin<PetscScalar>(_vec, dof);
    da->ReadFromGhostsEnd<PetscScalar>(_vec);

    unsigned int maxD = da->getMaxDepth();
    unsigned int lev;
    double hx, hy, hz;
    Point pt;

    double xFac = gSize[0]/((double)(1<<(maxD-1)));
    double yFac = gSize[1]/((double)(1<<(maxD-1)));
    double zFac = gSize[2]/((double)(1<<(maxD-1)));
    double xx, yy, zz;
    unsigned int idx[8];

    for ( da->init<ot::DA_FLAGS::WRITABLE>(); da->curr() < da->end<ot::DA_FLAGS::WRITABLE>(); da->next<ot::DA_FLAGS::WRITABLE>() ) { 
      // set the value
      lev = da->getLevel(da->curr());
      hx = xFac*(1<<(maxD - lev));
      hy = yFac*(1<<(maxD - lev));
      hz = zFac*(1<<(maxD - lev));

      pt = da->getCurrentOffset();

      xx = pt.x()*xFac; yy = pt.y()*yFac; zz = pt.z()*zFac;
			
      out << pt.x()*xFac << " " <<  pt.y()*yFac << " " << pt.z()*zFac << std::endl;
      out << pt.x()*xFac + hx << " " <<  pt.y()*yFac << " " << pt.z()*zFac << std::endl;
      out << pt.x()*xFac + hx << " " <<  pt.y()*yFac + hy << " " << pt.z()*zFac << std::endl;
      out << pt.x()*xFac << " " <<  pt.y()*yFac + hy << " " << pt.z()*zFac << std::endl;

      out << pt.x()*xFac << " " <<  pt.y()*yFac << " " << pt.z()*zFac + hz<< std::endl;
      out << pt.x()*xFac + hx << " " <<  pt.y()*yFac << " " << pt.z()*zFac + hz << std::endl;
      out << pt.x()*xFac + hx << " " <<  pt.y()*yFac + hy << " " << pt.z()*zFac + hz << std::endl;
      out << pt.x()*xFac << " " <<  pt.y()*yFac + hy << " " << pt.z()*zFac + hz << std::endl;
    }

    out << "CELLS " << da->getElementSize() << " " << num_cells_elements << std::endl;

    for (int i = 0; i < num_cells; i++) {
      out << unit_points << " ";
      for (int j = 0; j < unit_points; j++) {
        out << (i * unit_points + j) << " ";
      }
      out << std::endl;
    }

    out << "CELL_TYPES " << num_cells << std::endl;
    for (int i = 0; i < num_cells; i++) {
      out << VTK_HEXAHEDRON << std::endl;
    }

    //myfile<<"CELL_DATA "<<num_cells<<std::endl;
    //myfile<<"POINT_DATA "<<(num_cells*unit_points)<<std::endl;


    out << "FIELD OCTREE_DATA " << num_data_field << std::endl;

    out << "cell_level 1 " << num_cells << " int" << std::endl;

    for ( da->init<ot::DA_FLAGS::WRITABLE>(); da->curr() < da->end<ot::DA_FLAGS::WRITABLE>(); da->next<ot::DA_FLAGS::WRITABLE>() ) { 
      out << da->getLevel(da->curr()) << " ";
    }

    out << std::endl;

    out << "mpi_rank 1 " << num_cells << " int" << std::endl;
    for (int i = 0; i < num_cells; i++)
      out << rank << " ";

    out << std::endl;

    da->vecRestoreBuffer(vec,  _vec, false, false, true,  dof);
  }

  out.close();
}

