/*----------------------------------------------------------------------
  PuReMD - Purdue ReaxFF Molecular Dynamics Program

  Copyright (2010) Purdue University
  Hasan Metin Aktulga, hmaktulga@lbl.gov
  Joseph Fogarty, jcfogart@mail.usf.edu
  Sagar Pandit, pandit@usf.edu
  Ananth Y Grama, ayg@cs.purdue.edu

  Please cite the related publication:
  H. M. Aktulga, J. C. Fogarty, S. A. Pandit, A. Y. Grama,
  "Parallel Reactive Molecular Dynamics: Numerical Methods and
  Algorithmic Techniques", Parallel Computing, in press.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
  See the GNU General Public License for more details:
  <http://www.gnu.org/licenses/>.
  ----------------------------------------------------------------------*/

#include "pair_reaxc_sunway.h"
#include "reaxc_bonds_sunway.h"
#include "reaxc_bond_orders_sunway.h"
#include "reaxc_list_sunway.h"
#include "reaxc_tool_box_sunway.h"
#include "reaxc_vector_sunway.h"
#include "reaxc_bonds_sw64.h"
#include "gptl.h"

namespace REAXC_SUNWAY_NS{
extern"C"
{
  void Bonds_C(void *param);
  void Bonds_C_evflag(void *param);
}



void Bonds( reax_system *system, control_params *control,
            simulation_data *data, storage *workspace, reax_list **lists,
            output_controls *out_control )
{
  reax_system_c csys;
  system->to_c_sys(&csys);
 
  return;

  bonds_pack_t param;
  param.system      = &csys;
  param.control     = control;
  param.data        = data;
  param.workspace   = workspace;
  param.lists       = lists;
  param.out_control = out_control;
  GPTLstart("reaxc bonds c");
  Bonds_C(&param);
  GPTLstop("reaxc bonds c");
  
  system->from_c_sys(&csys);
  return;

  int i, j, pj, natoms;
  int start_i, end_i;
  int type_i, type_j;
  double ebond, pow_BOs_be2, exp_be12, CEbo;
  double gp3, gp4, gp7, gp10, gp37;
  double exphu, exphua1, exphub1, exphuov, hulpov, estriph;
  double decobdbo, decobdboua, decobdboub;
  single_body_parameters *sbp_i, *sbp_j;
  two_body_parameters *twbp;
  bond_order_data *bo_ij;
  reax_list *bonds;

  bonds = (*lists) + BONDS;
  double *Cdbo_list     = bonds->Cdbo_list;
  double *Cdbopi_list   = bonds->Cdbopi_list;
  double *Cdbopi2_list  = bonds->Cdbopi2_list;
  double *BO_list       = bonds->BO_list;
  rvec2 *BOpi_list      = bonds->BOpi_list;

  gp3 = system->reax_param.gp.l[3];
  gp4 = system->reax_param.gp.l[4];
  gp7 = system->reax_param.gp.l[7];
  gp10 = system->reax_param.gp.l[10];
  gp37 = (int) system->reax_param.gp.l[37];
  natoms = system->n;

  for( i = 0; i < natoms; ++i ) 
  {
    start_i = Start_Index(i, bonds);
    end_i = End_Index(i, bonds);

    for( pj = start_i; pj < end_i; ++pj ) 
    {
      j = bonds->select.bond_list[pj].nbr;

      if( system->packed_atoms[i].orig_id > system->packed_atoms[j].orig_id )
	      continue;
      if( system->packed_atoms[i].orig_id == system->packed_atoms[j].orig_id ) 
      {
        if (system->packed_atoms[j].x[2] <  system->packed_atoms[i].x[2]) continue;
      	if (system->packed_atoms[j].x[2] == system->packed_atoms[i].x[2] &&
      	    system->packed_atoms[j].x[1] <  system->packed_atoms[i].x[1]) continue;
        if (system->packed_atoms[j].x[2] == system->packed_atoms[i].x[2] &&
      	    system->packed_atoms[j].x[1] == system->packed_atoms[i].x[1] &&
      	    system->packed_atoms[j].x[0] <  system->packed_atoms[i].x[0]) continue;
      }

      /* set the pointers */
      type_i = system->packed_atoms[i].type;
      type_j = system->packed_atoms[j].type;
      sbp_i = &( system->reax_param.sbp[type_i] );
      sbp_j = &( system->reax_param.sbp[type_j] );
      twbp = &( system->reax_param.tbp[type_i][type_j] );
      //bo_ij = &( bonds->select.bond_list[pj].bo_data );
      bo_ij = &( bonds->bo_data_list[pj]);

      /* calculate the constants */
      if (bo_ij->BO_s == 0.0) pow_BOs_be2 = 0.0;
      else pow_BOs_be2 = pow( bo_ij->BO_s, twbp->p_be2 );
      exp_be12 = exp( twbp->p_be1 * ( 1.0 - pow_BOs_be2 ) );
      CEbo = -twbp->De_s * exp_be12 *
	        ( 1.0 - twbp->p_be1 * twbp->p_be2 * pow_BOs_be2 );

      /* calculate the Bond Energy */
      data->my_en.e_bond += ebond =
	      -twbp->De_s * bo_ij->BO_s * exp_be12
	      -twbp->De_p * BOpi_list[pj][0]
	      -twbp->De_pp * BOpi_list[pj][1];
        //-twbp->De_p * bo_ij->BO_pi
	      //-twbp->De_pp * bo_ij->BO_pi2;


      /* tally into per-atom energy */
      if( system->pair_ptr->evflag)
      {
	      //system->pair_ptr->ev_tally(i,j,natoms,1,ebond,0.0,0.0,0.0,0.0,0.0);
        system->pair_ptr->eng_vdwl += ebond;
      }

      /* calculate derivatives of Bond Orders */
      //bo_ij->Cdbo += CEbo;
      Cdbo_list[pj] += CEbo;

      //bo_ij->Cdbopi -= (CEbo + twbp->De_p);
      Cdbopi_list[pj] -= (CEbo + twbp->De_p);

      //bo_ij->Cdbopi2 -= (CEbo + twbp->De_pp);
      Cdbopi2_list[pj] -= (CEbo + twbp->De_pp);

      /* Stabilisation terminal triple bond */
      //if( bo_ij->BO >= 1.00 ) 
      if(BO_list[pj] >= 1.00 ) 
      {
	      if( gp37 == 2 ||
	      (sbp_i->mass == 12.0000 && sbp_j->mass == 15.9990) ||
	      (sbp_j->mass == 12.0000 && sbp_i->mass == 15.9990) ) 
        {
	        //exphu = exp( -gp7 * SQR(bo_ij->BO - 2.50) );
          //exphua1 = exp(-gp3 * (workspace->bo_dboc[i][0]-bo_ij->BO));
	        //exphub1 = exp(-gp3 * (workspace->bo_dboc[j][0]-bo_ij->BO));

	        exphu = exp( -gp7 * SQR(BO_list[pj] - 2.50) );
	        exphua1 = exp(-gp3 * (workspace->bo_dboc[i][0]-BO_list[pj]));
	        exphub1 = exp(-gp3 * (workspace->bo_dboc[j][0]-BO_list[pj]));
	        exphuov = exp(gp4 * (workspace->Delta[i] + workspace->Delta[j]));
	        hulpov = 1.0 / (1.0 + 25.0 * exphuov);

	        estriph = gp10 * exphu * hulpov * (exphua1 + exphub1);
	        data->my_en.e_bond += estriph;

	        decobdbo = gp10 * exphu * hulpov * (exphua1 + exphub1) *
	          ( gp3 - 2.0 * gp7 * (BO_list[pj]-2.50) );
	          //( gp3 - 2.0 * gp7 * (bo_ij->BO-2.50) );

	        decobdboua = -gp10 * exphu * hulpov *
	          (gp3*exphua1 + 25.0*gp4*exphuov*hulpov*(exphua1+exphub1));
	        decobdboub = -gp10 * exphu * hulpov *
	          (gp3*exphub1 + 25.0*gp4*exphuov*hulpov*(exphua1+exphub1));

	        /* tally into per-atom energy */
	        if( system->pair_ptr->evflag)
          {
	          //system->pair_ptr->ev_tally(i,j,natoms,1,estriph,0.0,0.0,0.0,0.0,0.0);
            system->pair_ptr->eng_vdwl += estriph;
          }

	        //bo_ij->Cdbo += decobdbo;
	        Cdbo_list[pj] += decobdbo;
	        workspace->fCdDelta[i][3] += decobdboua;
	        workspace->fCdDelta[j][3] += decobdboub;
	      }//if
      }//if
    }//for-pj
  }//for-i
}

}
