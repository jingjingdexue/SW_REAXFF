/* ----------------------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   http://lammps.sandia.gov, Sandia National Laboratories
   Steve Plimpton, sjplimp@sandia.gov

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

/* ----------------------------------------------------------------------
   Contributing author: Hasan Metin Aktulga, Purdue University
   (now at Lawrence Berkeley National Laboratory, hmaktulga@lbl.gov)
   Per-atom energy/virial added by Ray Shan (Sandia)
   Fix reax/c/bonds and fix reax/c/species for pair_style reax/c added by
   	Ray Shan (Sandia)
   Hybrid and hybrid/overlay compatibility added by Ray Shan (Sandia)
------------------------------------------------------------------------- */

#include "pair_reaxc_sunway.h"
#include "atom.h"
#include "update.h"
#include "force.h"
#include "comm.h"
#include "neighbor.h"
#include "neigh_list.h"
#include "neigh_request.h"
#include "modify.h"
#include "fix.h"
#include "fix_reaxc_sunway.h"
#include "citeme.h"
#include "memory.h"
#include "error.h"
#include "reaxc_types_sunway.h"
#include "reaxc_allocate_sunway.h"
#include "reaxc_control_sunway.h"
#include "reaxc_ffield_sunway.h"
#include "reaxc_forces_sunway.h"
#include "reaxc_init_md_sunway.h"
#include "reaxc_io_tools_sunway.h"
#include "reaxc_list_sunway.h"
#include "reaxc_lookup_sunway.h"
#include "reaxc_reset_tools_sunway.h"
#include "reaxc_traj_sunway.h"
#include "reaxc_vector_sunway.h"
#include "fix_reaxc_bonds_sunway.h"

#include "sunway.h"
#include "gptl.h"

#include "pair_reaxc_sw64.h"
using namespace LAMMPS_NS;
using namespace REAXC_SUNWAY_NS;

static const char cite_pair_reax_c[] =
  "pair reax/c command:\n\n"
  "@Article{Aktulga12,\n"
  " author = {H. M. Aktulga, J. C. Fogarty, S. A. Pandit, A. Y. Grama},\n"
  " title = {Parallel reactive molecular dynamics: Numerical methods and algorithmic techniques},\n"
  " journal = {Parallel Computing},\n"
  " year =    2012,\n"
  " volume =  38,\n"
  " pages =   {245--259}\n"
  "}\n\n";

/* ---------------------------------------------------------------------- */

PairReaxCSunway::PairReaxCSunway(LAMMPS *lmp) : Pair(lmp)
{
  if (lmp->citeme) lmp->citeme->add(cite_pair_reax_c);

  single_enable = 0;
  restartinfo = 0;
  one_coeff = 1;
  manybody_flag = 1;
  ghostneigh = 1;
  evflag = 0;
  system = (reax_system *)
    memory->smalloc(sizeof(reax_system),"reax:system");
  control = (control_params *)
    memory->smalloc(sizeof(control_params),"reax:control");
  data = (simulation_data *)
    memory->smalloc(sizeof(simulation_data),"reax:data");
  workspace = (storage *)
    memory->smalloc(sizeof(storage),"reax:storage");
  lists = (reax_list *)
    memory->smalloc(LIST_N * sizeof(reax_list),"reax:lists");
  memset(lists,0,LIST_N * sizeof(reax_list));
  out_control = (output_controls *)
    memory->smalloc(sizeof(output_controls),"reax:out_control");
  mpi_data = (mpi_datatypes *)
    memory->smalloc(sizeof(mpi_datatypes),"reax:mpi");

  MPI_Comm_rank(world,&system->my_rank);

  system->my_coords[0] = 0;
  system->my_coords[1] = 0;
  system->my_coords[2] = 0;
  system->num_nbrs = 0;
  system->n = 0; // my atoms
  system->N = 0; // mine + ghosts
  system->bigN = 0;  // all atoms in the system
  system->local_cap = 0;
  system->total_cap = 0;
  system->gcell_cap = 0;
  system->bndry_cuts.ghost_nonb = 0;
  system->bndry_cuts.ghost_hbond = 0;
  system->bndry_cuts.ghost_bond = 0;
  system->bndry_cuts.ghost_cutoff = 0;
  system->my_atoms = NULL;
  system->pair_ptr = this;

  system->omp_active = 0;

  fix_reax = NULL;
  tmpid = NULL;
  tmpbo = NULL;

  nextra = 14;
  pvector = new double[nextra];

  setup_flag = 0;
  fixspecies_flag = 0;

  nmax = 0;
}

/* ---------------------------------------------------------------------- */

PairReaxCSunway::~PairReaxCSunway()
{
  if (copymode) return;

  if (fix_reax) modify->delete_fix("REAXC");

  if (setup_flag) {
    Close_Output_Files( system, control, out_control, mpi_data );

    // deallocate reax data-structures

    //if( control->tabulate ) Deallocate_Lookup_Tables( system );

    if( control->hbond_cut > 0 )  Delete_List( lists+HBONDS, world );
    Delete_List( lists+BONDS, world );
    Delete_List( lists+THREE_BODIES, world );
    Delete_List( lists+FAR_NBRS_FULL, world );
    //Delete_List( lists+FAR_NBRS, world );

    DeAllocate_Workspace( control, workspace );
    DeAllocate_System( system );
  }

  memory->destroy( system );
  memory->destroy( control );
  memory->destroy( data );
  memory->destroy( workspace );
  memory->destroy( lists );
  memory->destroy( out_control );
  memory->destroy( mpi_data );

  // deallocate interface storage
  if( allocated ) {
    memory->destroy(setflag);
    memory->destroy(cutsq);
    memory->destroy(cutghost);
    delete [] map;

    delete [] chi;
    delete [] eta;
    delete [] gamma;
  }

  memory->destroy(tmpid);
  memory->destroy(tmpbo);

  delete [] pvector;

}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::allocate( )
{
  allocated = 1;
  int n = atom->ntypes;

  memory->create(setflag,n+1,n+1,"pair:setflag");
  memory->create(cutsq,n+1,n+1,"pair:cutsq");
  memory->create(cutghost,n+1,n+1,"pair:cutghost");
  map = new int[n+1];

  chi = new double[n+1];
  eta = new double[n+1];
  gamma = new double[n+1];
}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::settings(int narg, char **arg)
{
  if (narg < 1) error->all(FLERR,"Illegal pair_style command");

  // read name of control file or use default controls

  if (strcmp(arg[0],"NULL") == 0) {
    strcpy( control->sim_name, "simulate" );
    control->ensemble = 0;
    out_control->energy_update_freq = 0;
    control->tabulate = 0;

    control->reneighbor = 1;
    control->vlist_cut = control->nonb_cut;
    control->bond_cut = 5.;
    control->hbond_cut = 7.50;
    control->thb_cut = 0.001;
    control->thb_cutsq = 0.00001;
    control->bg_cut = 0.3;

    // Initialize for when omp style included
    control->nthreads = 1;

    out_control->write_steps = 0;
    out_control->traj_method = 0;
    strcpy( out_control->traj_title, "default_title" );
    out_control->atom_info = 0;
    out_control->bond_info = 0;
    out_control->angle_info = 0;
  } else Read_Control_File(arg[0], control, out_control);

  // default values

  qeqflag = 1;
  control->lgflag = 0;
  control->enobondsflag = 1;
  system->mincap = MIN_CAP;
  system->safezone = SAFE_ZONE;
  system->saferzone = SAFER_ZONE;
  system->maxfar = 1024;
  // process optional keywords

  int iarg = 1;

  while (iarg < narg) {
    if (strcmp(arg[iarg],"checkqeq") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style reax/c command");
      if (strcmp(arg[iarg+1],"yes") == 0) qeqflag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) qeqflag = 0;
      else error->all(FLERR,"Illegal pair_style reax/c command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"enobonds") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style reax/c command");
      if (strcmp(arg[iarg+1],"yes") == 0) control->enobondsflag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) control->enobondsflag = 0;
      else error->all(FLERR,"Illegal pair_style reax/c command");
      iarg += 2;
  } else if (strcmp(arg[iarg],"lgvdw") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style reax/c command");
      if (strcmp(arg[iarg+1],"yes") == 0) control->lgflag = 1;
      else if (strcmp(arg[iarg+1],"no") == 0) control->lgflag = 0;
      else error->all(FLERR,"Illegal pair_style reax/c command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"safezone") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style reax/c command");
      system->safezone = force->numeric(FLERR,arg[iarg+1]);
      if (system->safezone < 0.0)
	error->all(FLERR,"Illegal pair_style reax/c safezone command");
      system->saferzone = system->safezone*1.2 + 0.2;
      iarg += 2;
    } else if (strcmp(arg[iarg],"mincap") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style reax/c command");
      system->mincap = force->inumeric(FLERR,arg[iarg+1]);
      if (system->mincap < 0)
	error->all(FLERR,"Illegal pair_style reax/c mincap command");
      iarg += 2;
    } else if (strcmp(arg[iarg],"maxfar") == 0) {
      if (iarg+2 > narg) error->all(FLERR,"Illegal pair_style reax/c command");
      system->maxfar = force->inumeric(FLERR,arg[iarg+1]);
      if (system->maxfar < 0)
	error->all(FLERR,"Illegal pair_style reax/c maxfar command");
      iarg += 2;
    } else error->all(FLERR,"Illegal pair_style reax/c command");
  }

  // LAMMPS is responsible for generating nbrs

  control->reneighbor = 1;
}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::coeff( int nargs, char **args )
{
  if (!allocated) allocate();

  if (nargs != 3 + atom->ntypes)
    error->all(FLERR,"Incorrect args for pair coefficients");

  // insure I,J args are * *

  if (strcmp(args[0],"*") != 0 || strcmp(args[1],"*") != 0)
    error->all(FLERR,"Incorrect args for pair coefficients");

  // read ffield file

  char *file = args[2];
  FILE *fp;
  fp = force->open_potential(file);
  if (fp != NULL)
    Read_Force_Field(fp, &(system->reax_param), control);
  else {
      char str[128];
      sprintf(str,"Cannot open ReaxFF potential file %s",file);
      error->all(FLERR,str);
  }

  // read args that map atom types to elements in potential file
  // map[i] = which element the Ith atom type is, -1 if NULL

  int itmp = 0;
  int nreax_types = system->reax_param.num_atom_types;
  for (int i = 3; i < nargs; i++) {
    if (strcmp(args[i],"NULL") == 0) {
      map[i-2] = -1;
      itmp ++;
      continue;
    }
  }

  int n = atom->ntypes;

  // pair_coeff element map
  for (int i = 3; i < nargs; i++)
    for (int j = 0; j < nreax_types; j++)
      if (strcasecmp(args[i],system->reax_param.sbp[j].name) == 0) {
        map[i-2] = j;
	itmp ++;
      }

  // error check
  if (itmp != n)
    error->all(FLERR,"Non-existent ReaxFF type");

  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      setflag[i][j] = 0;

  // set setflag i,j for type pairs where both are mapped to elements

  int count = 0;
  for (int i = 1; i <= n; i++)
    for (int j = i; j <= n; j++)
      if (map[i] >= 0 && map[j] >= 0) {
        setflag[i][j] = 1;
        count++;
      }

  if (count == 0) error->all(FLERR,"Incorrect args for pair coefficients");

}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::init_style( )
{
  if (!atom->q_flag)
    error->all(FLERR,"Pair style reax/c requires atom attribute q");

  // firstwarn = 1;

  int iqeq;
  for (iqeq = 0; iqeq < modify->nfix; iqeq++)
    if (strstr(modify->fix[iqeq]->style,"qeq/reax")) break;
  if (iqeq == modify->nfix && qeqflag == 1)
    error->all(FLERR,"Pair reax/c requires use of fix qeq/reax");

  system->n = atom->nlocal; // my atoms
  system->N = atom->nlocal + atom->nghost; // mine + ghosts
  system->bigN = static_cast<int> (atom->natoms);  // all atoms in the system
  system->wsize = comm->nprocs;

  system->big_box.V = 0;
  system->big_box.box_norms[0] = 0;
  system->big_box.box_norms[1] = 0;
  system->big_box.box_norms[2] = 0;

  if (atom->tag_enable == 0)
    error->all(FLERR,"Pair style reax/c requires atom IDs");
  if (force->newton_pair == 0)
    error->all(FLERR,"Pair style reax/c requires newton pair on");

  // need a half neighbor list w/ Newton off and ghost neighbors
  // built whenever re-neighboring occurs

  int irequest;
  //irequest = neighbor->request(this,instance_me);
  //neighbor->requests[irequest]->id = 0;
  //neighbor->requests[irequest]->newton = 2;
  //neighbor->requests[irequest]->ghost = 1;

  irequest = neighbor->request(this,instance_me);
  neighbor->requests[irequest]->id = 1;
  neighbor->requests[irequest]->newton = 2;
  neighbor->requests[irequest]->ghost = 1;
  neighbor->requests[irequest]->half = 0;
  neighbor->requests[irequest]->full = 1;

  cutmax = MAX3(control->nonb_cut, control->hbond_cut, 2*control->bond_cut);

  for( int i = 0; i < LIST_N; ++i )
    lists[i].allocated = 0;

  if (fix_reax == NULL) {
    char **fixarg = new char*[3];
    fixarg[0] = (char *) "REAXC";
    fixarg[1] = (char *) "all";
    fixarg[2] = (char *) "REAXC";
    modify->add_fix(3,fixarg);
    delete [] fixarg;
    fix_reax = (FixReaxCSunway *) modify->fix[modify->nfix-1];
  }
}

void PairReaxCSunway::init_list(int which, NeighList *ptr){
  switch (which){
  case 0: list = ptr; break;
  case 1: listfull = ptr; break;
  }
}
/* ---------------------------------------------------------------------- */

void PairReaxCSunway::setup( )
{
  int oldN;
  int mincap = system->mincap;
  double safezone = system->safezone;

  system->n = atom->nlocal; // my atoms
  system->N = atom->nlocal + atom->nghost; // mine + ghosts
  oldN = system->N;
  system->bigN = static_cast<int> (atom->natoms);  // all atoms in the system

  if (setup_flag == 0) {

    setup_flag = 1;

    int *num_bonds = fix_reax->num_bonds;
    int *num_hbonds = fix_reax->num_hbonds;

    control->vlist_cut = neighbor->cutneighmax;

    // determine the local and total capacity

    system->local_cap = MAX( (int)(system->n * safezone), mincap );
    system->total_cap = MAX( (int)(system->N * safezone), mincap );

    // initialize my data structures

    PreAllocate_Space( system, control, workspace, world );
    write_reax_atoms();

    int num_nbrs = estimate_reax_lists();
    //if(!Make_List(system->total_cap, num_nbrs, TYP_FAR_NEIGHBOR,
    //              lists+FAR_NBRS, world))
    //  error->all(FLERR,"Pair reax/c problem in far neighbor list");
    if(!Make_List(system->total_cap, num_nbrs, TYP_FAR_NEIGHBOR_FULL,
                  lists+FAR_NBRS_FULL, world))
      error->all(FLERR,"Pair reax/c problem in far neighbor full list");

    write_reax_lists();
    write_reax_full_lists();
    Initialize( system, control, data, workspace, &lists, out_control,
                mpi_data, world );
  
        
    
    //2019/11/10
    if( !Make_List( system->Hcap, system->total_hbonds, TYP_HBOND, lists+HBONDS, world) ) 
    {
      fprintf( stderr, "not enough space for hbonds list. terminating!\n" );
      MPI_Abort(world, INSUFFICIENT_MEMORY );
    }
    
    
    //for( int k = 0; k < system->N; ++k ) 
    //{
    //  num_bonds[k] = system->my_atoms[k].num_bonds;
    //  num_hbonds[k] = system->my_atoms[k].num_hbonds;
    //}
    
    for( int k = 0; k < system->N; ++k ) 
    {
      num_bonds[k]  = system->num_bonds[k];
      num_hbonds[k] = system->num_hbonds[k];
    }

  } 
  else 
  {

    // fill in reax datastructures

    write_reax_atoms();

    //2019/11/10
    if( !Make_List( system->Hcap, system->total_hbonds, TYP_HBOND, lists+HBONDS, world) ) 
    {
      fprintf( stderr, "not enough space for hbonds list. terminating!\n" );
      MPI_Abort(world, INSUFFICIENT_MEMORY );
    }

    // reset the bond list info for new atoms

    for(int k = oldN; k < system->N; ++k)
      Set_End_Index( k, Start_Index( k, lists+BONDS ), lists+BONDS );

    // check if I need to shrink/extend my data-structs

    ReAllocate( system, control, data, workspace, &lists, mpi_data );
  }

  //bigint local_ngroup = list->inum;
  bigint local_ngroup = listfull->inum;
  MPI_Allreduce( &local_ngroup, &ngroup, 1, MPI_LMP_BIGINT, MPI_SUM, world );
}

/* ---------------------------------------------------------------------- */

double PairReaxCSunway::init_one(int i, int j)
{
  if (setflag[i][j] == 0) error->all(FLERR,"All pair coeffs are not set");

  cutghost[i][j] = cutghost[j][i] = cutmax;
  return cutmax;
}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::compute(int eflag, int vflag)
{
  double evdwl,ecoul;
  double t_start, t_end;
  int i;

  // communicate num_bonds once every reneighboring
  // 2 num arrays stored by fix, grab ptr to them

  GPTLstart("reaxc forward_comm");
  if (neighbor->ago == 0) comm->forward_comm_fix(fix_reax);
  GPTLstop("reaxc forward_comm");
  int *num_bonds = fix_reax->num_bonds;
  int *num_hbonds = fix_reax->num_hbonds;

  GPTLstart("reaxc ev setup");
  evdwl = ecoul = 0.0;
  if (eflag || vflag) ev_setup(eflag,vflag);
  else ev_unset();

  if (vflag_global) control->virial = 1;
  else control->virial = 0;

  GPTLstop("reaxc ev setup");
  system->x = atom->x;
  system->n = atom->nlocal; // my atoms
  system->N = atom->nlocal + atom->nghost; // mine + ghosts
  system->bigN = static_cast<int> (atom->natoms);  // all atoms in the system

  system->big_box.V = 0;
  system->big_box.box_norms[0] = 0;
  system->big_box.box_norms[1] = 0;
  system->big_box.box_norms[2] = 0;
  if( comm->me == 0 ) t_start = MPI_Wtime();

  // setup data structures
  GPTLstart("reaxc setup");
  setup();
  GPTLstop("reaxc setup");

  
  
  GPTLstart("reaxc reset");
  Reset( system, control, data, workspace, &lists, world );
  GPTLstop("reaxc reset");
  
  

  GPTLstart("reaxc write reax lists");
  workspace->realloc.num_far = write_reax_lists();
  GPTLstop("reaxc write reax lists");
  GPTLstart("reaxc write reax lists full");
  write_reax_full_lists();
  GPTLstop("reaxc write reax lists full");
  // timing for filling in the reax lists
  if( comm->me == 0 ) {
    t_end = MPI_Wtime();
    data->timing.nbrs = t_end - t_start;
  }

  
  //for(i = 0; i < system->total_cap; i++)
  //{
  //  workspace->fCdDelta[i][0] = workspace->f[i][0];
  //  workspace->fCdDelta[i][1] = workspace->f[i][1];
  //  workspace->fCdDelta[i][2] = workspace->f[i][2];
  //}
  
  // forces
  GPTLstart("reaxc compute forces");
  Compute_Forces(system,control,data,workspace,&lists,out_control,mpi_data);
  GPTLstop("reaxc compute forces");
    

  //GPTLstart("reaxc read forces");
  //read_reax_forces(vflag);
  //GPTLstop("reaxc read forces");
  //GPTLstart("reaxc copy bonds");
  //for(int k = 0; k < system->N; ++k) {
  //  num_bonds[k] = system->my_atoms[k].num_bonds;
  //  num_hbonds[k] = system->my_atoms[k].num_hbonds;
  //  system->my_atoms[k].f[0] = workspace->f[k][0];
  //  system->my_atoms[k].f[1] = workspace->f[k][1];
  //  system->my_atoms[k].f[2] = workspace->f[k][2];

  //  atom->f[k][0] += -workspace->f[k][0];
  //  atom->f[k][1] += -workspace->f[k][1];
  //  atom->f[k][2] += -workspace->f[k][2];

  //}
  //GPTLstop("reaxc copy bonds");
  // energies and pressure

  GPTLstart("reaxc eng press");
  if (eflag_global) {
    evdwl += data->my_en.e_bond;
    evdwl += data->my_en.e_ov;
    evdwl += data->my_en.e_un;
    evdwl += data->my_en.e_lp;
    evdwl += data->my_en.e_ang;
    evdwl += data->my_en.e_pen;
    evdwl += data->my_en.e_coa;
    evdwl += data->my_en.e_hb;
    evdwl += data->my_en.e_tor;
    evdwl += data->my_en.e_con;
    evdwl += data->my_en.e_vdW;

    ecoul += data->my_en.e_ele;
    ecoul += data->my_en.e_pol;

    // eng_vdwl += evdwl;
    // eng_coul += ecoul;

    // Store the different parts of the energy
    // in a list for output by compute pair command

    pvector[0] = data->my_en.e_bond;
    pvector[1] = data->my_en.e_ov + data->my_en.e_un;
    pvector[2] = data->my_en.e_lp;
    pvector[3] = 0.0;
    pvector[4] = data->my_en.e_ang;
    pvector[5] = data->my_en.e_pen;
    pvector[6] = data->my_en.e_coa;
    pvector[7] = data->my_en.e_hb;
    pvector[8] = data->my_en.e_tor;
    pvector[9] = data->my_en.e_con;
    pvector[10] = data->my_en.e_vdW;
    pvector[11] = data->my_en.e_ele;
    pvector[12] = 0.0;
    pvector[13] = data->my_en.e_pol;
  }
  
  
  if (vflag_fdotr) 
  {
    //virial_fdotr_compute();

    if (neighbor->includegroup == 0) 
    {
      int nall = atom->nlocal + atom->nghost;
      for (int i = 0; i < nall; i++) 
      {
        //num_bonds[i] = system->my_atoms[i].num_bonds;
        //num_hbonds[i] = system->my_atoms[i].num_hbonds;
        
        num_bonds[i]  = system->num_bonds[i];
        num_hbonds[i] = system->num_hbonds[i];

        //system->my_atoms[i].f[0] = workspace->f[i][0];
        //system->my_atoms[i].f[1] = workspace->f[i][1];
        //system->my_atoms[i].f[2] = workspace->f[i][2];
        atom->f[i][0] += -workspace->fCdDelta[i][0];
        atom->f[i][1] += -workspace->fCdDelta[i][1];
        atom->f[i][2] += -workspace->fCdDelta[i][2];

        virial[0] += atom->f[i][0]*atom->x[i][0];
        virial[1] += atom->f[i][1]*atom->x[i][1];
        virial[2] += atom->f[i][2]*atom->x[i][2];
        virial[3] += atom->f[i][1]*atom->x[i][0];
        virial[4] += atom->f[i][2]*atom->x[i][0];
        virial[5] += atom->f[i][2]*atom->x[i][1];
      }
    } 
    else 
    {//need to validate
      printf("neighbor->includegroup=%d\n", neighbor->includegroup);
      
      int nall = atom->nlocal + atom->nghost;
      for (int i = atom->nlocal; i < nall; i++) 
      {
        //num_bonds[i] = system->my_atoms[i].num_bonds;
        //num_hbonds[i] = system->my_atoms[i].num_hbonds;
        num_bonds[i]  = system->num_bonds[i];
        num_hbonds[i] = system->num_hbonds[i];

        //system->my_atoms[i].f[0] = workspace->f[i][0];
        //system->my_atoms[i].f[1] = workspace->f[i][1];
        //system->my_atoms[i].f[2] = workspace->f[i][2];
        atom->f[i][0] += -workspace->fCdDelta[i][0];
        atom->f[i][1] += -workspace->fCdDelta[i][1];
        atom->f[i][2] += -workspace->fCdDelta[i][2];

        virial[0] += atom->f[i][0]*atom->x[i][0];
        virial[1] += atom->f[i][1]*atom->x[i][1];
        virial[2] += atom->f[i][2]*atom->x[i][2];
        virial[3] += atom->f[i][1]*atom->x[i][0];
        virial[4] += atom->f[i][2]*atom->x[i][0];
        virial[5] += atom->f[i][2]*atom->x[i][1];
      }
      
      nall = atom->nfirst;
      for (int i = 0; i < nall; i++) 
      {
        virial[0] += atom->f[i][0]*atom->x[i][0];
        virial[1] += atom->f[i][1]*atom->x[i][1];
        virial[2] += atom->f[i][2]*atom->x[i][2];
        virial[3] += atom->f[i][1]*atom->x[i][0];
        virial[4] += atom->f[i][2]*atom->x[i][0];
        virial[5] += atom->f[i][2]*atom->x[i][1];
      }
    }

    vflag_fdotr = 0;
  }
  else
  {
    for(int k = 0; k < system->N; ++k) 
    {
      //num_bonds[k] = system->my_atoms[k].num_bonds;
      //num_hbonds[k] = system->my_atoms[k].num_hbonds;
      num_bonds[i]  = system->num_bonds[i];
      num_hbonds[i] = system->num_hbonds[i];

      //system->my_atoms[k].f[0] = workspace->f[k][0];
      //system->my_atoms[k].f[1] = workspace->f[k][1];
      //system->my_atoms[k].f[2] = workspace->f[k][2];

      atom->f[k][0] += -workspace->fCdDelta[k][0];
      atom->f[k][1] += -workspace->fCdDelta[k][1];
      atom->f[k][2] += -workspace->fCdDelta[k][2];
    }

  }
  GPTLstop("reaxc eng press");
// Set internal timestep counter to that of LAMMPS

  data->step = update->ntimestep;
  GPTLstart("reaxc output results");
  Output_Results( system, control, data, &lists, out_control, mpi_data );
  GPTLstop("reaxc output results");
  // populate tmpid and tmpbo arrays for fix reax/c/species
  int j;

  GPTLstart("reaxc findbond");
  if(fixspecies_flag) {
    if (system->N > nmax) {
      memory->destroy(tmpid);
      memory->destroy(tmpbo);
      nmax = system->N;
      memory->create(tmpid,nmax,MAXSPECBOND,"pair:tmpid");
      memory->create(tmpbo,nmax,MAXSPECBOND,"pair:tmpbo");
    }

    for (i = 0; i < system->N; i ++)
      for (j = 0; j < MAXSPECBOND; j ++) 
      {
        tmpbo[i][j] = 0.0;
	      tmpid[i][j] = 0;
      }
    FindBond();
  }
  GPTLstop("reaxc findbond");
}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::write_reax_atoms()
{
  int *num_bonds = fix_reax->num_bonds;
  int *num_hbonds = fix_reax->num_hbonds;
  //printf("%lld\n", system);
  if (system->N > system->total_cap)
    error->all(FLERR,"Too many ghost atoms");
  reax_system_c csys;
  system->to_c_sys(&csys);
  //printf("%lld\n", system->my_atoms);
  write_atoms_param_t pm;
  pm.map = map;
  pm.csys = &csys;
  pm.tag = atom->tag;
  pm.type = atom->type;
  pm.ntypes = atom->ntypes;
  pm.x = static_cast<double (*)[3]>((void*)atom->x[0]);
  pm.q = atom->q;
  pm.num_bonds = num_bonds;
  pm.num_hbonds = num_hbonds;
  GPTLstart("write reax atoms and pack");
  write_reax_atoms_and_pack(&pm);
  GPTLstop("write reax atoms and pack");
  return;
  //for( int i = 0; i < system->N; ++i ){
  //  system->my_atoms[i].orig_id = atom->tag[i];
  //  system->my_atoms[i].type = map[atom->type[i]];
  //  system->my_atoms[i].x[0] = atom->x[i][0];
  //  system->my_atoms[i].x[1] = atom->x[i][1];
  //  system->my_atoms[i].x[2] = atom->x[i][2];
  //  system->my_atoms[i].q = atom->q[i];
  //  system->my_atoms[i].num_bonds = num_bonds[i];
  //  system->my_atoms[i].num_hbonds = num_hbonds[i];
  //}
}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::get_distance( rvec xj, rvec xi, double *d_sqr, rvec *dvec )
{
  (*dvec)[0] = xj[0] - xi[0];
  (*dvec)[1] = xj[1] - xi[1];
  (*dvec)[2] = xj[2] - xi[2];
  *d_sqr = SQR((*dvec)[0]) + SQR((*dvec)[1]) + SQR((*dvec)[2]);
}

/* ---------------------------------------------------------------------- */

//void PairReaxCSunway::set_far_nbr( far_neighbor_data *fdest,
//                              int j, double d, rvec dvec )
//{
//  fdest->nbr = j;
//  fdest->d = d;
//  rvec_Copy( fdest->dvec, dvec );
//  ivec_MakeZero( fdest->rel_box );
//}

/* ---------------------------------------------------------------------- */

int PairReaxCSunway::estimate_reax_lists()
{
  //return system->N * system->maxfar / 2;
  return system->N * system->maxfar;
}
/* ---------------------------------------------------------------------- */
  
int PairReaxCSunway::write_reax_lists()
{
  return system->N * system->maxfar / 2;
}
/* ---------------------------------------------------------------------- */
int PairReaxCSunway::write_reax_full_lists()
{
  int itr_i, itr_j, i, j;
  int num_nbrs;//, num_marked;
  int *ilist, *jlist, *numneigh, **firstneigh;//, *marked;
  double d_sqr;
  rvec dvec;
  double dist, **x;
  reax_list *far_nbrs;
  far_neighbor_data_full *far_list, far_nbrs_fulllist;

  x = atom->x;
  ilist = listfull->ilist;
  numneigh = listfull->numneigh;
  firstneigh = listfull->firstneigh;

  far_nbrs = lists + FAR_NBRS_FULL;
  far_list = far_nbrs->select.far_nbr_list_full;

  num_nbrs = 0;

  int numall = listfull->inum + listfull->gnum;//
  
  write_lists_param_t pm;
  pm.numall = numall;
  pm.x = static_cast<double(*)[3]>((void*)atom->x[0]);
  pm.patoms = system->packed_atoms;
  pm.ilist = ilist;
  pm.numneigh = numneigh;
  pm.firstneigh = firstneigh;
  pm.nonb_cutsq = control->nonb_cut * control->nonb_cut;
  pm.far_nbrs = far_nbrs;
  pm.maxfar = system->maxfar;
  pm.far_nbr_list_full = far_nbrs->select.far_nbr_list_full;
  pm.index = far_nbrs->index;
  pm.end_index = far_nbrs->end_index;
  
  GPTLstart("reaxc write reax lists full c");
  write_reax_lists_c(&pm);
  GPTLstop("reaxc write reax lists full c");

  return 0;



  for( itr_i = 0; itr_i < numall; ++itr_i ){
    i = ilist[itr_i];
    jlist = firstneigh[i];
    Set_Start_Index( i, num_nbrs, far_nbrs );

    for( itr_j = 0; itr_j < numneigh[i]; ++itr_j ){
      j = jlist[itr_j];
      j &= NEIGHMASK;
      get_distance( system->packed_atoms[j].x, system->packed_atoms[i].x, &d_sqr, &dvec );
      if( d_sqr <= (control->nonb_cut*control->nonb_cut) ){
        far_neighbor_data_full *nbr = far_list + num_nbrs;
        nbr->nbr = j;
        nbr->d = sqrt(d_sqr);
        rvec_Copy( nbr->dvec, dvec );
        ivec_MakeZero( nbr->rel_box );
        nbr->type = system->packed_atoms[j].type;
        nbr->orig_id = system->packed_atoms[j].orig_id;
        nbr->q = system->packed_atoms[j].q;
        ++num_nbrs;
      }
    }
    Set_End_Index( i, num_nbrs, far_nbrs );
  }

  return num_nbrs;

  //int mincap = system->mincap;
  //double safezone = system->safezone;

  //x = atom->x;
  //ilist = list->ilist;
  //numneigh = list->numneigh;
  //firstneigh = list->firstneigh;

  //num_nbrs = 0;
  ////num_marked = 0;
  ////marked = (int*) calloc( system->N, sizeof(int) );

  //int numall = list->inum + list->gnum;
  //int num_allnbrs = 0;
  //for( itr_i = 0; itr_i < numall; ++itr_i ){
  //  i = ilist[itr_i];
  //  num_allnbrs += numneigh[i];
  //  // marked[i] = 1;
  //  // ++num_marked;
  //  jlist = firstneigh[i];

  //  for( itr_j = 0; itr_j < numneigh[i]; ++itr_j ){
  //    j = jlist[itr_j];
  //    j &= NEIGHMASK;
  //    get_distance( x[j], x[i], &d_sqr, &dvec );

  //    if( d_sqr <= SQR(control->nonb_cut) )
  //      ++num_nbrs;
  //  }
  //}
  ////free( marked );

  //return static_cast<int> (MAX( num_nbrs*safezone, mincap*MIN_NBRS ));
}

/* ---------------------------------------------------------------------- */

//int PairReaxCSunway::write_reax_lists()
//{
//  int itr_i, itr_j, i, j;
//  int num_nbrs;
//  int *ilist, *jlist, *numneigh, **firstneigh;
//  double d_sqr;
//  rvec dvec;
//  double *dist, **x;
//  reax_list *far_nbrs, *far_nbrs_full;
//  far_neighbor_data *far_list, far_nbrs_fulllist;
//
//  x = atom->x;
//  ilist = list->ilist;
//  numneigh = list->numneigh;
//  firstneigh = list->firstneigh;
//
//  far_nbrs = lists + FAR_NBRS;
//  far_list = far_nbrs->select.far_nbr_list;
//
//  num_nbrs = 0;
//  dist = (double*) calloc( system->N, sizeof(double) );
//
//  int numall = list->inum + list->gnum;
//
//  for( itr_i = 0; itr_i < numall; ++itr_i )
//  {
//    i = ilist[itr_i];
//    jlist = firstneigh[i];
//    Set_Start_Index( i, num_nbrs, far_nbrs );
//
//    for( itr_j = 0; itr_j < numneigh[i]; ++itr_j )
//    {
//      j = jlist[itr_j];
//      j &= NEIGHMASK;
//      get_distance( x[j], x[i], &d_sqr, &dvec );
//      if( d_sqr <= (control->nonb_cut*control->nonb_cut) )
//      {
//        dist[j] = sqrt( d_sqr );
//        set_far_nbr( &far_list[num_nbrs], j, dist[j], dvec );
//        ++num_nbrs;
//      }
//    }
//    Set_End_Index( i, num_nbrs, far_nbrs );
//  }
//
//  free( dist );
//  return num_nbrs;
//}
//
//int PairReaxCSunway::write_reax_full_lists()
//{
//  int itr_i, itr_j, i, j;
//  int num_nbrs;
//  int *ilist, *jlist, *numneigh, **firstneigh;
//  double d_sqr;
//  rvec dvec;
//  double dist, **x;
//  reax_list *far_nbrs;
//  far_neighbor_data_full *far_list, far_nbrs_fulllist;
//
//  x = atom->x;
//  ilist = listfull->ilist;
//  numneigh = listfull->numneigh;
//  firstneigh = listfull->firstneigh;
//
//  far_nbrs = lists + FAR_NBRS_FULL;
//  far_list = far_nbrs->select.far_nbr_list_full;
//
//  num_nbrs = 0;
//
//  int numall = list->inum + list->gnum;
//  
//  write_lists_param_t pm;
//  pm.numall = numall;
//  pm.x = static_cast<double(*)[3]>((void *) atom->x[0]);
//  pm.patoms = system->packed_atoms;
//  pm.ilist = ilist;
//  pm.numneigh = numneigh;
//  pm.firstneigh = firstneigh;
//  pm.nonb_cutsq = control->nonb_cut * control->nonb_cut;
//  pm.far_nbrs = far_nbrs;
//  pm.maxfar = system->maxfar;
//  write_reax_lists_c(&pm);
//  return 0;
//  for( itr_i = 0; itr_i < numall; ++itr_i ){
//    i = ilist[itr_i];
//    jlist = firstneigh[i];
//    Set_Start_Index( i, num_nbrs, far_nbrs );
//
//    for( itr_j = 0; itr_j < numneigh[i]; ++itr_j ){
//      j = jlist[itr_j];
//      j &= NEIGHMASK;
//      get_distance( system->packed_atoms[j].x, system->packed_atoms[i].x, &d_sqr, &dvec );
//      if( d_sqr <= (control->nonb_cut*control->nonb_cut) ){
//        far_neighbor_data_full *nbr = far_list + num_nbrs;
//        nbr->nbr = j;
//        nbr->d = sqrt(d_sqr);
//        rvec_Copy( nbr->dvec, dvec );
//        ivec_MakeZero( nbr->rel_box );
//        nbr->type = system->packed_atoms[j].type;
//        nbr->orig_id = system->packed_atoms[j].orig_id;
//        nbr->q = system->packed_atoms[j].q;
//        ++num_nbrs;
//      }
//    }
//    Set_End_Index( i, num_nbrs, far_nbrs );
//  }
//
//  return num_nbrs;
//}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::read_reax_forces(int vflag)
{
  for( int i = 0; i < system->N; ++i ) {
    //system->my_atoms[i].f[0] = workspace->f[i][0];
    //system->my_atoms[i].f[1] = workspace->f[i][1];
    //system->my_atoms[i].f[2] = workspace->f[i][2];

    atom->f[i][0] += -workspace->fCdDelta[i][0];
    atom->f[i][1] += -workspace->fCdDelta[i][1];
    atom->f[i][2] += -workspace->fCdDelta[i][2];
    
  }

}

/* ---------------------------------------------------------------------- */

void *PairReaxCSunway::extract(const char *str, int &dim)
{
  dim = 1;
  if (strcmp(str,"chi") == 0 && chi) {
    for (int i = 1; i <= atom->ntypes; i++)
      if (map[i] >= 0) chi[i] = system->reax_param.sbp[map[i]].chi;
      else chi[i] = 0.0;
    return (void *) chi;
  }
  if (strcmp(str,"eta") == 0 && eta) {
    for (int i = 1; i <= atom->ntypes; i++)
      if (map[i] >= 0) eta[i] = system->reax_param.sbp[map[i]].eta;
      else eta[i] = 0.0;
    return (void *) eta;
  }
  if (strcmp(str,"gamma") == 0 && gamma) {
    for (int i = 1; i <= atom->ntypes; i++)
      if (map[i] >= 0) gamma[i] = system->reax_param.sbp[map[i]].gamma;
      else gamma[i] = 0.0;
    return (void *) gamma;
  }
  return NULL;
}

/* ---------------------------------------------------------------------- */

double PairReaxCSunway::memory_usage()
{
  double bytes = 0.0;

  // From pair_reax_c
  bytes += 1.0 * system->N * sizeof(int);
  bytes += 1.0 * system->N * sizeof(double);

  // From reaxc_allocate: BO
  bytes += 1.0 * system->total_cap * sizeof(reax_atom);
  bytes += 1.0 * system->total_cap * sizeof(atom_pack_t);
  bytes += 1.0 * system->total_cap * sizeof(int);//Hindex
  bytes += 19.0 * system->total_cap * sizeof(double);
  bytes += 3.0 * system->total_cap * sizeof(int);

  // From reaxc_lists
  bytes += 2.0 * lists->n * sizeof(int);
  //bytes += lists->num_intrs * sizeof(three_body_interaction_data);
  bytes += lists->num_intrs * sizeof(bond_data);
  bytes += lists->num_intrs * sizeof(double);//Cdbo_list
  bytes += lists->num_intrs * sizeof(double);//Cdbopi_list
  bytes += lists->num_intrs * sizeof(double);//Cdbopi2_list
  bytes += lists->num_intrs * sizeof(double);//BO_list
  bytes += lists->num_intrs * sizeof(double) * 2;//BOpi_list
  bytes += lists->num_intrs * sizeof(bond_order_data);//bo_data_list
  //bytes += lists->num_intrs * sizeof(dbond_data);
  //bytes += lists->num_intrs * sizeof(dDelta_data);

  bytes += lists->num_intrs * sizeof(far_neighbor_data_full);
  bytes += lists->num_intrs * sizeof(hbond_data);

  if(fixspecies_flag)
    bytes += 2 * nmax * MAXSPECBOND * sizeof(double);
  return bytes;
}

/* ---------------------------------------------------------------------- */

void PairReaxCSunway::FindBond()
{
  int i, j, pj, nj;
  double bo_tmp, bo_cut;
  double *BO_list = lists->BO_list;

  bond_data *bo_ij;
  bo_cut = 0.10;

  for (i = 0; i < system->n; i++) {
    nj = 0;
    for( pj = Start_Index(i, lists); pj < End_Index(i, lists); ++pj ) {
      bo_ij = &( lists->select.bond_list[pj] );
      j = bo_ij->nbr;
      if (j < i) continue;

      //bo_tmp = bo_ij->bo_data.BO;
      bo_tmp = BO_list[pj];

      if (bo_tmp >= bo_cut ) 
      {
	      tmpid[i][nj] = j;
	      tmpbo[i][nj] = bo_tmp;
	      nj ++;
	      if (nj > MAXSPECBOND) error->all(FLERR,"Increase MAXSPECBOND in reaxc_defs_sunway.h");
      }
    }
  }
}

