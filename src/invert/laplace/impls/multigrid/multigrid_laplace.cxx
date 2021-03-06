/**************************************************************************
 * Perpendicular Laplacian inversion. 
 *                           Using Geometrical Multigrid Solver
 *
 * Equation solved is:
 *  d*\nabla^2_\perp x + (1/c1)\nabla_perp c2\cdot\nabla_\perp x + a x = b
 *
 **************************************************************************
 * Copyright 2016 K.S. Kang
 *
 * Contact: Ben Dudson, bd512@york.ac.uk
 * 
 * This file is part of BOUT++.
 *
 * BOUT++ is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BOUT++ is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with BOUT++.  If not, see <http://www.gnu.org/licenses/>.
 *
 **************************************************************************/

#include "multigrid_laplace.hxx"

BoutReal soltime=0.0,settime=0.0;


LaplaceMultigrid::LaplaceMultigrid(Options *opt) : 
  Laplacian(opt),
  A(0.0), C1(1.0), C2(1.0), D(1.0)
{

  // Get Options in Laplace Section
  if (!opt) opts = Options::getRoot()->getSection("laplace");
  else opts=opt;
  opts->get("multigridlevel",mglevel,7,true);
  opts->get("rtol",rtol,pow(10.0,-8),true);
  opts->get("atol",atol,pow(10.0,-20),true);
  opts->get("dtol",dtol,pow(10.0,5),true);
  opts->get("smtype",mgsm,1,true);
  opts->get("jacomega",omega,0.8,true);
  opts->get("solvertype",mgplag,1,true);
  opts->get("cftype",cftype,0,true);
  opts->get("mergempi",mgmpi,63,true);
  opts->get("checking",pcheck,0,true);
  tcheck = pcheck;
  mgcount = 0;

  // Initialize, allocate memory, etc.
  comms_tagbase = 385; // Some random number
  
  int implemented_global_flags = INVERT_START_NEW;
  if ( global_flags & ~implemented_global_flags ) {
    throw BoutException("Attempted to set Laplacian inversion flag that is not implemented in LaplaceMultigrid.");
  }
  int implemented_boundary_flags = INVERT_AC_GRAD + INVERT_SET + INVERT_DC_GRAD; // INVERT_DC_GRAD does not actually do anything, but harmless to set while comparing to Fourier solver with Neumann boundary conditions
  if ( inner_boundary_flags & ~implemented_boundary_flags ) {
    throw BoutException("Attempted to set Laplacian inner boundary inversion flag that is not implemented in LaplaceMultigrid.");
  }
  if ( outer_boundary_flags & ~implemented_boundary_flags ) {
    throw BoutException("Attempted to set Laplacian outer boundary inversion flag that is not implemented in LaplaceMultigrid.");
  }
  if (nonuniform) {
    throw BoutException("nonuniform option is not implemented in LaplaceMultigrid.");
  }
  
  commX = mesh->getXcomm();
  
  Nx_local = mesh->xend - mesh->xstart + 1; // excluding guard cells
  Nx_global = mesh->GlobalNx - 2*mesh->xstart; // excluding guard cells
  
  if(mgcount == 0) {
    output <<"Nx="<<Nx_global<<"("<<Nx_local<<")"<<endl;
  }
  Nz_global = mesh->GlobalNz - 1;
           // z-direction has one extra, unused point for historical reasons
  Nz_local = Nz_global; // No parallelization in z-direction (for now)
  // 
  //else {
  //  Nz_local = mesh->zend - mesh->zstart + 1; // excluding guard cells
  //  Nz_global = mesh->GlobalNz - 2*mesh->zstart; // excluding guard cells
  // }
  if(mgcount==0) {
    output <<"Nz="<<Nz_global<<"("<<Nz_local<<")"<<endl;
  }


  // Compute available levels along x-direction
  int aclevel,adlevel;
  if(mglevel >1) {
    int nn = Nx_local;
    aclevel = mglevel;
    for(int n = aclevel;n > 1; n--) {
      if ( nn%2 != 0 )  {
	output<<"Size of local x-domain is not a power of 2^"<<mglevel<<" mglevel is changed to"<<mglevel-n+1<<endl;
        aclevel = aclevel - n + 1;
        n = 1;
      }
      nn = nn/2;
    }
    nn = Nz_local;
    for(int n = aclevel;n > 1; n--) {
      if ( nn%2 != 0 )  {
	output<<"Size of local z-domain is not a power of 2^ "<<aclevel <<" mglevel is changed to "<<aclevel - n + 1<<endl;
        aclevel = aclevel - n + 1;
        n = 1;
      }
      nn = nn/2;
    }
  }
  else aclevel = 1;
  adlevel = mglevel - aclevel;

  int rcheck = 0;
  if((pcheck == 1) && (mgcount == 0)) rcheck = 1;
  kMG = new Multigrid1DP(aclevel,Nx_local,Nz_local,Nx_global,adlevel,mgmpi,
            commX,rcheck);
  kMG->mgplag = mgplag;
  kMG->mgsm = mgsm; 
  kMG->cftype = cftype;
  kMG->rtol = rtol;
  kMG->atol = atol;
  kMG->dtol = dtol;
  kMG->omega = omega;
  kMG->setValueS();

  // Set up Multigrid Cycle

  x = new BoutReal[(Nx_local+2)*(Nz_local+2)];
  b = new BoutReal[(Nx_local+2)*(Nz_local+2)];

  if(mgcount == 0) {  
    output<<" Smoothing type is ";
    if(mgsm == 0) {
      output<<"Jacobi smoother";
      output<<"with omega = "<<omega<<endl;
    }
    else if(mgsm ==1) output<<" Gauss-Seidel smoother"<<endl;
    else throw BoutException("Undefined smoother");
    output<<"Solver type is ";
    if(mglevel == 1) output<<"PGMRES with simple Preconditioner"<<endl;
    else if(mgplag == 1) output<<"PGMRES with multigrid Preconditioner"<<endl;
    else output<<"Multigrid solver with merging "<<mgmpi<<endl;
#ifdef OPENMP
#pragma omp parallel
#pragma omp master
    {
      output<<"Num threads = "<<omp_get_num_threads()<<endl;
    } 
#endif
  }  
}

LaplaceMultigrid::~LaplaceMultigrid() {
  // Finalize, deallocate memory, etc.
  delete [] x;
  delete [] b;
  kMG->cleanMem();
  kMG->cleanS();
  kMG = NULL;
}

const FieldPerp LaplaceMultigrid::solve(const FieldPerp &b_in, const FieldPerp &x0) {

  BoutReal t0,t1;
  
  yindex = b_in.getIndex();
  int level = kMG->mglevel-1;
  int lzz = kMG->lnz[level];
  int lz2 = lzz+2;
  int lxx = kMG->lnx[level];

  if ( global_flags & INVERT_START_NEW ) {
    // set initial guess to zero
    for (int i=1; i<lxx+1; i++) {
      int i2 = i-1+mesh->xstart;
#pragma omp parallel default(shared) 
#pragma omp for
      for (int k=1; k<lzz+1; k++) {
        int k2 = k;
        x[i*lz2+k] = 0.;
      }
    }
  }
  else {
    // Read initial guess into local array, ignoring guard cells
    for (int i=1; i<lxx+1; i++) {
      int i2 = i-1+mesh->xstart;
#pragma omp parallel default(shared) 
#pragma omp for
      for (int k=1; k<lzz+1; k++) {
        int k2 = k-1;
        x[i*lz2+k] = x0[i2][k2];
      }
    }
  }
  
  // Read RHS into local array
  for (int i=1; i<lxx+1; i++) {
    int i2 = i-1+mesh->xstart;
    for (int k=1; k<lzz+1; k++) {
      int k2 = k-1;
      b[i*lz2+k] = b_in[i2][k2];
    }
  }
  
  if (mesh->firstX()) {
    if ( inner_boundary_flags & INVERT_AC_GRAD ) {
      // Neumann boundary condition
      if ( inner_boundary_flags & INVERT_SET ) {
        // guard cells of x0 specify gradient to set at inner boundary
        for (int k=1; k<lzz+1; k++) {
          int k2 = k-1;
	  x[k] = -x0[mesh->xstart-1][k2]*sqrt(mesh->g_11[mesh->xstart][yindex])*mesh->dx[mesh->xstart][yindex]; 
        }
      }
      else {
        // zero gradient inner boundary condition
        for (int k=1; k<lzz+1; k++) {
          // set inner guard cells
          x[k] = 0.0;
        }
      }
    }
    else {
      // Dirichlet boundary condition
      if ( inner_boundary_flags & INVERT_SET ) {
        // guard cells of x0 specify value to set at inner boundary
        for (int k=1; k<lzz+1; k++) {
          int k2 = k-1;
          x[k] = 2.*x0[mesh->xstart-1][k2]; 
        // this is the value to set at the inner boundary
        }
      }
      else {
        // zero value inner boundary condition
        for (int k=1; k<lzz+1; k++) {
          // set inner guard cells
          x[k] = 0.;
        }
      }
    }
  }
  if (mesh->lastX()) {
    if ( outer_boundary_flags & INVERT_AC_GRAD ) {
      // Neumann boundary condition
      if ( inner_boundary_flags & INVERT_SET ) {
        // guard cells of x0 specify gradient to set at outer boundary
        for (int k=1; k<lzz+1; k++) {
          int k2 = k-1;
        x[(lxx+1)*lz2+k] = x0[mesh->xend+1][k2]*sqrt(mesh->g_11[mesh->xend][yindex])*mesh->dx[mesh->xend][yindex]; 
        // this is the value to set the gradient to at the outer boundary
        }
      }
      else {
        // zero gradient outer boundary condition
        for (int k=1; k<lzz+1; k++) {
          // set outer guard cells
          x[(lxx+1)*lz2+k] = 0.;
        }
      }
    }
    else {
      // Dirichlet boundary condition
      if ( outer_boundary_flags & INVERT_SET ) {
        // guard cells of x0 specify value to set at outer boundary
        for (int k=1; k<lzz+1; k++) {
          int k2 = k-1;
          x[(lxx+1)*lz2+k]=2.*x0[mesh->xend+1][k2]; 
          // this is the value to set at the outer boundary
        }
      }
      else {
        // zero value inner boundary condition
        for (int k=1; k<lzz+1; k++) {
          // set outer guard cells
          x[(lxx+1)*lz2+k] = 0.;
        }
      }
    }
  }

  // Exchange ghost cells of initial guess
  for(int i=0;i<lxx+2;i++) {
    x[i*lz2] = x[(i+1)*lz2-2];
    x[(i+1)*lz2-1] = x[i*lz2+1];
  }
   
  if(mgcount == 0) {
    soltime = 0.0;
    settime = 0.0;
  }
  else {
    if(kMG->pcheck > 0) {
      kMG->setPcheck(0);
    }
  }
  

  t0 = MPI_Wtime();
  generateMatrixF(level);  

  if(kMG->xNP > 1) MPI_Barrier(commX);

  if((pcheck == 3) && (mgcount == 0)) {
    FILE *outf;
    char outfile[256];
    sprintf(outfile,"test_matF_%d.mat",kMG->rProcI);
    output<<"Out file= "<<outfile<<endl;
    outf = fopen(outfile,"w");
    int dim =  (lxx+2)*(lzz+2);
    fprintf(outf,"dim = %d (%d, %d)\n",dim,lxx,lzz);

    for(int i = 0;i<dim;i++) {
      fprintf(outf,"%d ==",i);
      for(int j=0;j<9;j++) fprintf(outf,"%12.6f,",kMG->matmg[level][i*9+j]);
      fprintf(outf,"\n");
    }  
    fclose(outf);
  }

  if(level > 0) kMG->setMultigridC(0);

  if((pcheck == 3) && (mgcount == 0)) {
    for(int i = level; i> 0;i--) {
      output<<i<<"dimension= "<<kMG->lnx[i-1]<<"("<<kMG->gnx[i-1]<<"),"<<kMG->lnz[i-1]<<endl;
      
      FILE *outf;
      char outfile[256];
      sprintf(outfile,"test_matC%1d_%d.mat",i,kMG->rProcI);
      output<<"Out file= "<<outfile<<endl;
      outf = fopen(outfile,"w");
      int dim =  (kMG->lnx[i-1]+2)*(kMG->lnz[i-1]+2);
      fprintf(outf,"dim = %d (%d,%d)\n",dim,kMG->lnx[i-1],kMG->lnz[i-1]);
  
      for(int ii = 0;ii<dim;ii++) {
        fprintf(outf,"%d ==",ii);
        for(int j=0;j<9;j++) fprintf(outf,"%12.6f,",kMG->matmg[i-1][ii*9+j]);
        fprintf(outf,"\n");
      }  
      fclose(outf);
    }
  }

  t1 = MPI_Wtime();
  settime += t1-t0;
  // Compute solution.

  mgcount++;
  if((mgcount == 300) && (pcheck == 0)) {
    tcheck = 1;
  }
  t0 = MPI_Wtime();

  kMG->getSolution(x,b,0);

  t1 = MPI_Wtime();
  if((mgcount == 300) && (tcheck != pcheck)) tcheck = pcheck;
  soltime += t1-t0;
  if(mgcount%300 == 0) {
    output<<"Accumulated execution time at "<<mgcount<<" Sol "<<soltime<<" ( "<<settime<<" )"<<endl;
  }
  
  FieldPerp result;
  result.allocate();
  #if CHECK>2
    // Make any unused elements NaN so that user does not try to do calculations with them
    result = 1./0.;
  #endif
  // Copy solution into a FieldPerp to return
  for (int i=1; i<lxx+1; i++) {
    int i2 = i-1+mesh->xstart;
    for (int k=1; k<lzz+1; k++) {
      int k2 = k-1;
      result[i2][k2] = x[i*lz2+k];
    }
  }
  if (xProcI == 0) {
    if ( inner_boundary_flags & INVERT_AC_GRAD ) {
      // Neumann boundary condition
      int i2 = -1+mesh->xstart;
      for (int k=1; k<lzz+1; k++) {
        int k2 = k-1;
        result[i2][k2] = x[lz2+k] - x[k];
      }
    }
    else {
      // Dirichlet boundary condition
      int i2 = -1+mesh->xstart;
      for (int k=1; k<lzz+1; k++) {
        int k2 = k-1;
        result[i2][k2] = x[k]- x[lz2+k];
      }
    }
  }
  if (xProcI == xNP-1) {
    if ( outer_boundary_flags & INVERT_AC_GRAD ) {
      // Neumann boundary condition
      int i2 = lxx+mesh->xstart;
      for (int k=1; k<lzz+1; k++) {
        int k2 = k-1;
        result[i2][k2] = x[lxx*lz2+k]-x[(lxx+1)*lz2+k];
      }
    }
    else {
      // Dirichlet boundary condition
      int i2 = lxx+mesh->xstart;
      for (int k=1; k<lzz+1; k++) {
        int k2 = k-1;
        result[i2][k2] = x[(lxx+1)*lz2+k]-x[lxx*lz2+k];
      }
    }
  }
  result.setIndex(yindex); // Set the index of the FieldPerp to be returned
  
  return result;
  
}


void LaplaceMultigrid::generateMatrixF(int level) {

  // Set (fine-level) matrix entries

  int i2,k2;
  BoutReal *mat;
  mat = kMG->matmg[level];
  int llx = kMG->lnx[level];
  int llz = kMG->lnz[level];

  for (int i=1; i<llx+1; i++) {
    i2 = i-1+mesh->xstart;
#pragma omp parallel default(shared) private(k2)
#pragma omp for
    for (int k=1; k<llz+1; k++) {
      k2 = k-1;
      int k2p  = (k2+1)%Nz_global;
      int k2m  = (k2+Nz_global-1)%Nz_global;
      
      BoutReal ddx_C = (C2[i2+1][yindex][k2] - C2[i2-1][yindex][k2])/2./mesh->dx[i2][yindex]/C1[i2][yindex][k2];
      BoutReal ddz_C = (C2[i2][yindex][k2p] - C2[i2][yindex][k2m]) /2./mesh->dz/C1[i2][yindex][k2];
      
      BoutReal ddx = D[i2][yindex][k2]*mesh->g11[i2][yindex]/mesh->dx[i2][yindex]/mesh->dx[i2][yindex]; 
               // coefficient of 2nd derivative stencil (x-direction)
      
      BoutReal ddz = D[i2][yindex][k2]*mesh->g33[i2][yindex]/mesh->dz/mesh->dz; 
              // coefficient of 2nd derivative stencil (z-direction)
      
      BoutReal dxdz = D[i2][yindex][k2]*mesh->g13[i2][yindex]/mesh->dx[i2][yindex]/mesh->dz/2.; 
              // coefficient of mixed derivative stencil (could assume zero, at least initially, 
              // if easier; then check this is true in constructor)
      
      BoutReal dxd = (D[i2][yindex][k2]*2.*mesh->G1[i2][yindex]
        + mesh->g11[i2][yindex]*ddx_C
        + mesh->g13[i2][yindex]*ddz_C // (could assume zero, at least initially, if easier; then check this is true in constructor)
      )/mesh->dx[i2][yindex]; // coefficient of 1st derivative stencil (x-direction)
      
      BoutReal dzd = (D[i2][yindex][k2]*2.*mesh->G3[i2][yindex]
        + mesh->g33[i2][yindex]*ddz_C
        + mesh->g13[i2][yindex]*ddx_C // (could assume zero, at least initially, if easier; then check this is true in constructor)
      )/mesh->dz; // coefficient of 1st derivative stencil (z-direction)
      
      int ic = i*(llz+2)+k;
      mat[ic*9] = dxdz/4.;
      mat[ic*9+1] = ddx - dxd/2.;
      mat[ic*9+2] = -dxdz/4.;
      mat[ic*9+3] = ddz - dzd/2.;
      mat[ic*9+4] = A[i2][yindex][k2] - 2.*(ddx+ddz); // coefficient of no-derivative component
      mat[ic*9+5] = ddz + dzd/2.;
      mat[ic*9+6] = -dxdz/4.;
      mat[ic*9+7] = ddx+dxd/2.;
      mat[ic*9+8] = dxdz/4.;
    }
  }

  // Here put boundary conditions

  if (kMG->rProcI == 0) {
    if ( inner_boundary_flags & INVERT_AC_GRAD ) {
      // Neumann boundary condition
      for(int k = 1;k<llz+1; k++) {
        int ic = llz+2 +k;
        mat[ic*9+3] += mat[ic*9];
        mat[ic*9+4] += mat[ic*9+1];
        mat[ic*9+5] += mat[ic*9+2];
        b[ic] -= mat[ic*9]*x[k-1];
        b[ic] -= mat[ic*9+1]*x[k];
        b[ic] -= mat[ic*9+2]*x[k+1];
        mat[ic*9] = 0.;
        mat[ic*9+1] = 0.;
        mat[ic*9+2] = 0.;
      }
    }
    else {
      // Dirichlet boundary condition
      for(int k = 1;k<llz+1; k++) {
        int ic = llz+2 +k;
        mat[ic*9+3] -= mat[ic*9];
        mat[ic*9+4] -= mat[ic*9+1];
        mat[ic*9+5] -= mat[ic*9+2];
        b[ic] -= mat[ic*9]*x[k-1];
        b[ic] -= mat[ic*9+1]*x[k];
        b[ic] -= mat[ic*9+2]*x[k+1];
        mat[ic*9] = 0.;
        mat[ic*9+1] = 0.;
        mat[ic*9+2] = 0.;
      }
    }
  }
  if (kMG->rProcI == kMG->xNP-1) {
    if ( outer_boundary_flags & INVERT_AC_GRAD ) {
      // Neumann boundary condition
      for(int k = 1;k<llz+1; k++) {
        int ic = llx*(llz+2)+k;
        mat[ic*9+3] += mat[ic*9+6];
        mat[ic*9+4] += mat[ic*9+7];
        mat[ic*9+5] += mat[ic*9+8];
        b[ic] -= mat[ic*9+6]*x[(llx+1)*(llz+2)+k-1];
        b[ic] -= mat[ic*9+7]*x[(llx+1)*(llz+2)+k];
        b[ic] -= mat[ic*9+8]*x[(llx+1)*(llz+2)+k+1];
        mat[ic*9+6] = 0.;
        mat[ic*9+7] = 0.;
        mat[ic*9+8] = 0.;
      }
    }
    else {
      // Dirichlet boundary condition
      for(int k = 1;k<llz+1; k++) {
        int ic = llx*(llz+2)+k;
        mat[ic*9+3] -= mat[ic*9+6];
        mat[ic*9+4] -= mat[ic*9+7];
        mat[ic*9+5] -= mat[ic*9+8];
        b[ic] -= mat[ic*9+6]*x[(llx+1)*(llz+2)+k-1];
        b[ic] -= mat[ic*9+7]*x[(llx+1)*(llz+2)+k];
        b[ic] -= mat[ic*9+8]*x[(llx+1)*(llz+2)+k+1];
        mat[ic*9+6] = 0.;
        mat[ic*9+7] = 0.;
        mat[ic*9+8] = 0.;
      }
    }
  }
}

