#include "esolver_ks_lcao.h"

//--------------temporary----------------------------
#include "../src_pw/global.h"
#include "../module_base/global_function.h"
#include "../src_io/print_info.h"
#include "src_lcao/dftu.h"
#include "src_lcao/dmft.h"
#include "input_update.h"
#include "src_pw/occupy.h"
#include "src_lcao/ELEC_evolve.h"
#include "src_lcao/ELEC_cbands_gamma.h"
#include "src_lcao/ELEC_cbands_k.h"
#include "src_pw/symmetry_rho.h"
#include "src_io/chi0_hilbert.h"
#include "src_pw/threshold_elec.h"

#ifdef __DEEPKS
#include "../module_deepks/LCAO_deepks.h"
#endif
//-----force& stress-------------------
#include "src_lcao/FORCE_STRESS.h"


//---------------------------------------------------

namespace ModuleESolver
{

ESolver_KS_LCAO::ESolver_KS_LCAO()
{
    classname = "ESolver_KS_LCAO";
    basisname = "LCAO";
}
ESolver_KS_LCAO::~ESolver_KS_LCAO()
{
    this->orb_con.clear_after_ions(GlobalC::UOT, GlobalC::ORB, GlobalV::deepks_setorb, GlobalC::ucell.infoNL.nproj);
}

void ESolver_KS_LCAO::Init(Input& inp, UnitCell_pseudo& ucell)
{    
    // setup GlobalV::NBANDS
	// Yu Liu add 2021-07-03
	GlobalC::CHR.cal_nelec();

	// it has been established that that
	// xc_func is same for all elements, therefore
	// only the first one if used
	if(ucell.atoms[0].xc_func=="HSE" || ucell.atoms[0].xc_func=="PBE0")
	{
		XC_Functional::set_xc_type("pbe");
	}
	else
	{
		XC_Functional::set_xc_type(ucell.atoms[0].xc_func);
	}

    //ucell.setup_cell( GlobalV::global_pseudo_dir , GlobalV::stru_file , GlobalV::ofs_running, GlobalV::NLOCAL, GlobalV::NBANDS);
    ModuleBase::GlobalFunc::DONE(GlobalV::ofs_running, "SETUP UNITCELL");

    // symmetry analysis should be performed every time the cell is changed
    if (ModuleSymmetry::Symmetry::symm_flag)
    {
        GlobalC::symm.analy_sys(ucell, GlobalV::ofs_running);
        ModuleBase::GlobalFunc::DONE(GlobalV::ofs_running, "SYMMETRY");
    }

    // Setup the k points according to symmetry.
    GlobalC::kv.set(GlobalC::symm, GlobalV::global_kpoint_card, GlobalV::NSPIN, ucell.G, ucell.latvec );
    ModuleBase::GlobalFunc::DONE(GlobalV::ofs_running,"INIT K-POINTS");

    // print information
    // mohan add 2021-01-30
    Print_Info::setup_parameters(ucell, GlobalC::kv);

//--------------------------------------
// cell relaxation should begin here
//--------------------------------------

    // Initalize the plane wave basis set
    GlobalC::pw.gen_pw(GlobalV::ofs_running, ucell, GlobalC::kv);
    ModuleBase::GlobalFunc::DONE(GlobalV::ofs_running,"INIT PLANEWAVE");
    std::cout << " UNIFORM GRID DIM     : " << GlobalC::pw.nx <<" * " << GlobalC::pw.ny <<" * "<< GlobalC::pw.nz << std::endl;
    std::cout << " UNIFORM GRID DIM(BIG): " << GlobalC::pw.nbx <<" * " << GlobalC::pw.nby <<" * "<< GlobalC::pw.nbz << std::endl;

    // initialize the real-space uniform grid for FFT and parallel
    // distribution of plane waves
    GlobalC::Pgrid.init(GlobalC::pw.ncx, GlobalC::pw.ncy, GlobalC::pw.ncz, GlobalC::pw.nczp,
        GlobalC::pw.nrxx, GlobalC::pw.nbz, GlobalC::pw.bz); // mohan add 2010-07-22, update 2011-05-04
	// Calculate Structure factor
    GlobalC::pw.setup_structure_factor();

	// Inititlize the charge density.
    GlobalC::CHR.allocate(GlobalV::NSPIN, GlobalC::pw.nrxx, GlobalC::pw.ngmc);
    ModuleBase::GlobalFunc::DONE(GlobalV::ofs_running,"INIT CHARGE");

	// Initializee the potential.
    GlobalC::pot.allocate(GlobalC::pw.nrxx);
    ModuleBase::GlobalFunc::DONE(GlobalV::ofs_running,"INIT POTENTIAL");

#ifdef __MPI 
	if(GlobalV::CALCULATION=="nscf")
	{
		switch(GlobalC::exx_global.info.hybrid_type)
		{
			case Exx_Global::Hybrid_Type::HF:
			case Exx_Global::Hybrid_Type::PBE0:
			case Exx_Global::Hybrid_Type::HSE:
				XC_Functional::set_xc_type(ucell.atoms[0].xc_func);
				break;
		}
	}
#endif

#ifdef __DEEPKS
	//wenfei 2021-12-19
	//if we are performing DeePKS calculations, we need to load a model
	if (GlobalV::deepks_scf)
	{
		// load the DeePKS model from deep neural network
		GlobalC::ld.load_model(INPUT.deepks_model);
	}
#endif

    // Initialize the local wave functions.
    // npwx, eigenvalues, and weights
    // npwx may change according to cell change
    // this function belongs to cell LOOP
    GlobalC::wf.allocate_ekb_wg(GlobalC::kv.nks);

    // Initialize the FFT.
    // this function belongs to cell LOOP
    GlobalC::UFFT.allocate();

    // output is GlobalC::ppcell.vloc 3D local pseudopotentials
	// without structure factors
    // this function belongs to cell LOOP
    GlobalC::ppcell.init_vloc(GlobalC::pw.nggm, GlobalC::ppcell.vloc);

    // Initialize the sum of all local potentials.
    // if ion_step==0, read in/initialize the potentials
    // this function belongs to ions LOOP
    int ion_step=0;
    GlobalC::pot.init_pot(ion_step, GlobalC::pw.strucFac);

#ifdef __MPI  
	// PLEASE simplify the Exx_Global interface
	// mohan add 2021-03-25
	// Peize Lin 2016-12-03
	if (GlobalV::CALCULATION=="scf" || GlobalV::CALCULATION=="relax" || GlobalV::CALCULATION=="cell-relax")
	{
		switch(GlobalC::exx_global.info.hybrid_type)
		{
			case Exx_Global::Hybrid_Type::HF:
			case Exx_Global::Hybrid_Type::PBE0:
			case Exx_Global::Hybrid_Type::HSE:
				GlobalC::exx_lcao.init();
				break;
			case Exx_Global::Hybrid_Type::No:
			case Exx_Global::Hybrid_Type::Generate_Matrix:
				break;
			default:
				throw std::invalid_argument(ModuleBase::GlobalFunc::TO_STRING(__FILE__)+ModuleBase::GlobalFunc::TO_STRING(__LINE__));
		}
	}	
#endif

	// PLEASE do not use INPUT global variable
	// mohan add 2021-03-25
	// Quxin added for DFT+U
	if(INPUT.dft_plus_u) 
	{
		GlobalC::dftu.init(ucell, this->LM);
	}

    if (INPUT.dft_plus_dmft) GlobalC::dmft.init(INPUT, ucell);
    
    //------------------init Basis_lcao----------------------
    // Init Basis should be put outside of Ensolver.
    // * reading the localized orbitals/projectors
    // * construct the interpolation tables.
    this->Init_Basis_lcao(this->orb_con, inp, ucell);
    //------------------init Basis_lcao----------------------

    //------------------init Hamilt_lcao----------------------
    // * allocate H and S matrices according to computational resources
    // * set the 'trace' between local H/S and global H/S
    this->LM.divide_HS_in_frag(GlobalV::GAMMA_ONLY_LOCAL, orb_con.ParaV);
    //------------------init Hamilt_lcao----------------------
  //init Psi
    if (GlobalV::GAMMA_ONLY_LOCAL)
        this->LOWF.wfc_gamma.resize(GlobalV::NSPIN);
    else
        this->LOWF.wfc_k.resize(GlobalC::kv.nks);
    //pass Hamilt-pointer to Operator
    this->UHM.genH.LM = this->UHM.LM = &this->LM;
    //pass basis-pointer to EState and Psi
    this->LOC.ParaV = this->LOWF.ParaV = this->LM.ParaV;
}

void ESolver_KS_LCAO::cal_Energy(energy &en)
{
    
}

void ESolver_KS_LCAO::cal_Force(ModuleBase::matrix &force)
{
    
    Force_Stress_LCAO FSL(this->RA);
    FSL.getForceStress(GlobalV::CAL_FORCE, GlobalV::CAL_STRESS,
        GlobalV::TEST_FORCE, GlobalV::TEST_STRESS,
        this->LOC, this->LOWF, this->UHM, force, this->scs);
    //delete RA after cal_Force
    this->RA.delete_grid();
}
void ESolver_KS_LCAO::cal_Stress(ModuleBase::matrix &stress)
{
    //copy the stress
    stress = this->scs;
}
void ESolver_KS_LCAO::postprocess()
{
    GlobalC::en.perform_dos(this->LOWF,this->UHM);
}

void ESolver_KS_LCAO::Init_Basis_lcao(ORB_control& orb_con, Input& inp, UnitCell_pseudo& ucell)
{
    // Set the variables first
    this->orb_con.gamma_only = GlobalV::GAMMA_ONLY_LOCAL;
    this->orb_con.nlocal = GlobalV::NLOCAL;
    this->orb_con.nbands = GlobalV::NBANDS;
    this->orb_con.ParaV.nspin = GlobalV::NSPIN;
    this->orb_con.dsize = GlobalV::DSIZE;
    this->orb_con.nb2d = GlobalV::NB2D;
    this->orb_con.dcolor = GlobalV::DCOLOR;
    this->orb_con.drank = GlobalV::DRANK;
    this->orb_con.myrank = GlobalV::MY_RANK;
    this->orb_con.calculation = GlobalV::CALCULATION;
    this->orb_con.ks_solver = GlobalV::KS_SOLVER;
    this->orb_con.setup_2d = true;

    // * reading the localized orbitals/projectors
    // * construct the interpolation tables.
    this->orb_con.read_orb_first(
        GlobalV::ofs_running,
        GlobalC::ORB,
        ucell.ntype,
        ucell.lmax,
        inp.lcao_ecut,
        inp.lcao_dk,
        inp.lcao_dr,
        inp.lcao_rmax,
        GlobalV::deepks_setorb,
        inp.out_mat_r,
        GlobalV::CAL_FORCE,
        GlobalV::MY_RANK);

    ucell.infoNL.setupNonlocal(
        ucell.ntype,
		ucell.atoms,
		GlobalV::ofs_running,
        GlobalC::ORB);

#ifdef __MPI   
	this->orb_con.set_orb_tables(
		GlobalV::ofs_running,
		GlobalC::UOT,
		GlobalC::ORB,
		ucell.lat0,
		GlobalV::deepks_setorb,
		Exx_Abfs::Lmax,
		ucell.infoNL.nprojmax,
		ucell.infoNL.nproj,
        ucell.infoNL.Beta);
#else
	int Lmax=0;
	this->orb_con.set_orb_tables(
		GlobalV::ofs_running,
		GlobalC::UOT,
		GlobalC::ORB,
		ucell.lat0,
		GlobalV::deepks_setorb,
		Lmax,
		ucell.infoNL.nprojmax,
	    ucell.infoNL.nproj,
        ucell.infoNL.Beta);
#endif

    if (this->orb_con.setup_2d)
        this->orb_con.setup_2d_division(GlobalV::ofs_running, GlobalV::ofs_warning);
}


void ESolver_KS_LCAO::eachiterinit(const int istep, const int iter)
{
    std::string ufile = "CHANGE";
    Update_input UI;
    UI.init(ufile);

    if (INPUT.dft_plus_u) GlobalC::dftu.iter_dftu = iter;
    
    // mohan add 2010-07-16
    // used for pulay mixing.
    if(iter==1)
    {
        GlobalC::CHR.set_new_e_iteration(true);
    }
    else
    {
        GlobalC::CHR.set_new_e_iteration(false);
    }

    if(GlobalV::FINAL_SCF && iter==1)
    {
        GlobalC::CHR.irstep=0;
        GlobalC::CHR.idstep=0;
        GlobalC::CHR.totstep=0;
    }

    // mohan update 2012-06-05
    GlobalC::en.calculate_harris(1);

    // mohan move it outside 2011-01-13
    // first need to calculate the weight according to
    // electrons number.
    // mohan add iter > 1 on 2011-04-02
    // because the GlobalC::en.ekb has not value now.
    // so the smearing can not be done.
    if(iter>1)Occupy::calculate_weights();

    if(GlobalC::wf.init_wfc == "file")
    {
        if(iter==1)
        {
            std::cout << " WAVEFUN -> CHARGE " << std::endl;

            // The occupation should be read in together.
            // Occupy::calculate_weights(); //mohan add 2012-02-15

            // calculate the density matrix using read in wave functions
            // and the ncalculate the charge density on grid.
            this->LOC.sum_bands(this->UHM);
            // calculate the local potential(rho) again.
            // the grid integration will do in later grid integration.


            // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            // a puzzle remains here.
            // if I don't renew potential,
            // The scf_thr is very small.
            // OneElectron, Hartree and
            // Exc energy are all correct
            // except the band energy.
            //
            // solved by mohan 2010-09-10
            // there are there rho here:
            // rho1: formed by read in orbitals.
            // rho2: atomic rho, used to construct H
            // rho3: generated by after diagonalize
            // here converged because rho3 and rho1
            // are very close.
            // so be careful here, make sure
            // rho1 and rho2 are the same rho.
            // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            GlobalC::pot.vr = GlobalC::pot.v_of_rho(GlobalC::CHR.rho, GlobalC::CHR.rho_core);
            GlobalC::en.delta_escf();
            if (ELEC_evolve::td_vext == 0)
            {
                GlobalC::pot.set_vr_eff();
            }
            else
            {
                GlobalC::pot.set_vrs_tddft(istep);
            }
        }
    }

#ifdef __MPI
    // calculate exact-exchange
    if(XC_Functional::get_func_type()==4)
    {
        if( !GlobalC::exx_global.info.separate_loop )
        {
            GlobalC::exx_lcao.cal_exx_elec(this->LOC, this->LOWF.wfc_k_grid);
        }
    }
#endif

    if(INPUT.dft_plus_u)
    {
        GlobalC::dftu.cal_slater_UJ(istep, iter); // Calculate U and J if Yukawa potential is used
    }
}
void ESolver_KS_LCAO::hamilt2density(int istep, int iter, double ethr) 
{
    // (1) calculate the bands.
    // mohan add 2021-02-09
    if(GlobalV::GAMMA_ONLY_LOCAL)
    {
        ELEC_cbands_gamma::cal_bands(istep, this->UHM, this->LOWF, this->LOC.dm_gamma);
    }
    else
    {
        if(ELEC_evolve::tddft && istep >= 1 && iter > 1)
        {
            ELEC_evolve::evolve_psi(istep, this->UHM, this->LOWF);
        }
        else
        {
            ELEC_cbands_k::cal_bands(istep, this->UHM, this->LOWF, this->LOC.dm_k);
        }
    }
    //-----------------------------------------------------------
    // only deal with charge density after both wavefunctions.
    // are calculated.
    //-----------------------------------------------------------
    if(GlobalV::GAMMA_ONLY_LOCAL && GlobalV::NSPIN == 2 && GlobalV::CURRENT_SPIN == 0) return;

    GlobalC::en.eband  = 0.0;
    GlobalC::en.ef     = 0.0;
    GlobalC::en.ef_up  = 0.0;
    GlobalC::en.ef_dw  = 0.0;

    // demet is included into eband.
    //if(GlobalV::DIAGO_TYPE!="selinv")
    {
        GlobalC::en.demet  = 0.0;
    }

    // (2)
    GlobalC::CHR.save_rho_before_sum_band();

    // (3) sum bands to calculate charge density
    Occupy::calculate_weights();

    if (GlobalV::ocp == 1)
    {
        for (int ik=0; ik<GlobalC::kv.nks; ik++)
        {
            for (int ib=0; ib<GlobalV::NBANDS; ib++)
            {
                GlobalC::wf.wg(ik,ib)=GlobalV::ocp_kb[ik*GlobalV::NBANDS+ib];
            }
        }
    }


    for(int ik=0; ik<GlobalC::kv.nks; ++ik)
    {
        GlobalC::en.print_band(ik);
    }

    // if selinv is used, we need this to calculate the charge
    // using density matrix.
    this->LOC.sum_bands(this->UHM);

#ifdef __MPI
    // add exx
    // Peize Lin add 2016-12-03
    GlobalC::en.set_exx();

    // Peize Lin add 2020.04.04
    if(XC_Functional::get_func_type()==4)
    {
        if(GlobalC::restart.info_load.load_H && GlobalC::restart.info_load.load_H_finish && !GlobalC::restart.info_load.restart_exx)
        {
            XC_Functional::set_xc_type(GlobalC::ucell.atoms[0].xc_func);
            GlobalC::exx_lcao.cal_exx_elec(this->LOC, this->LOWF.wfc_k_grid);
            GlobalC::restart.info_load.restart_exx = true;
        }
    }
#endif

    // if DFT+U calculation is needed, this function will calculate
    // the local occupation number matrix and energy correction
    if(INPUT.dft_plus_u)
    {
        if(GlobalV::GAMMA_ONLY_LOCAL) GlobalC::dftu.cal_occup_m_gamma(iter, this->LOC.dm_gamma);
        else GlobalC::dftu.cal_occup_m_k(iter, this->LOC.dm_k);

        GlobalC::dftu.cal_energy_correction(istep);
        GlobalC::dftu.output();
    }
    
#ifdef __DEEPKS
    if(GlobalV::deepks_scf) 
    {
        const Parallel_Orbitals* pv = this->LOWF.ParaV;
        if (GlobalV::GAMMA_ONLY_LOCAL)
        {
            GlobalC::ld.cal_e_delta_band(this->LOC.dm_gamma,
                pv->trace_loc_row, pv->trace_loc_col, pv->nrow);
        }
        else
        {
            GlobalC::ld.cal_e_delta_band_k(this->LOC.dm_k,
                pv->trace_loc_row, pv->trace_loc_col, 
                GlobalC::kv.nks, pv->nrow, pv->ncol);
        }
    }
#endif
    // (4) mohan add 2010-06-24
    // using new charge density.
    GlobalC::en.calculate_harris(2);

    // (5) symmetrize the charge density
    Symmetry_rho srho;
    for(int is=0; is<GlobalV::NSPIN; is++)
    {
        srho.begin(is, GlobalC::CHR,GlobalC::pw, GlobalC::Pgrid, GlobalC::symm);
    }

    // (6) compute magnetization, only for spin==2
    GlobalC::ucell.magnet.compute_magnetization();

    // resume codes!
    //-------------------------------------------------------------------------
    // this->GlobalC::LOWF.init_Cij( 0 ); // check the orthogonality of local orbital.
    // GlobalC::CHR.sum_band(); use local orbital in plane wave basis to calculate bands.
    // but must has evc first!
    //-------------------------------------------------------------------------

    // (7) calculate delta energy
    GlobalC::en.deband = GlobalC::en.delta_e();
}
void ESolver_KS_LCAO::updatepot(const int istep, const int iter, const bool conv)
{
    // (9) Calculate new potential according to new Charge Density.

    if(conv || iter==GlobalV::SCF_NMAX)
    {
        if(GlobalC::pot.out_pot<0) //mohan add 2011-10-10
        {
            GlobalC::pot.out_pot = -2;
        }
    }
    if (!conv)
    {
        GlobalC::pot.vr = GlobalC::pot.v_of_rho(GlobalC::CHR.rho, GlobalC::CHR.rho_core);
        GlobalC::en.delta_escf();
    }
    else
    {
        GlobalC::pot.vnew = GlobalC::pot.v_of_rho(GlobalC::CHR.rho, GlobalC::CHR.rho_core);
        //(used later for scf correction to the forces )
        GlobalC::pot.vnew -= GlobalC::pot.vr;
        GlobalC::en.descf = 0.0;
    }

    // add Vloc to Vhxc.
    if(ELEC_evolve::td_vext == 0)
    {
        GlobalC::pot.set_vr_eff();
    }
    else
    {
        GlobalC::pot.set_vrs_tddft(istep);
    }
}
void ESolver_KS_LCAO::eachiterfinish(int iter, bool conv)
{
    //-----------------------------------
    // save charge density
    //-----------------------------------
    // Peize Lin add 2020.04.04
    if(GlobalC::restart.info_save.save_charge)
    {
        for(int is=0; is<GlobalV::NSPIN; ++is)
        {
            GlobalC::restart.save_disk(*this->UHM.LM, "charge", is);
        }
    }
    //-----------------------------------
    // output charge density for tmp
    //-----------------------------------
    for(int is=0; is<GlobalV::NSPIN; is++)
    {
        const int precision = 3;

        std::stringstream ssc;
        ssc << GlobalV::global_out_dir << "tmp" << "_SPIN" << is + 1 << "_CHG";
        GlobalC::CHR.write_rho(GlobalC::CHR.rho_save[is], is, iter, ssc.str(), precision );//mohan add 2007-10-17

        std::stringstream ssd;

        if(GlobalV::GAMMA_ONLY_LOCAL)
        {
            ssd << GlobalV::global_out_dir << "tmp" << "_SPIN" << is + 1 << "_DM";
        }
        else
        {
            ssd << GlobalV::global_out_dir << "tmp" << "_SPIN" << is + 1 << "_DM_R";
        }
        this->LOC.write_dm( is, iter, ssd.str(), precision );
    }

    // (11) calculate the total energy.
    GlobalC::en.calculate_etot();

    GlobalC::en.etot_old = GlobalC::en.etot;

}
void ESolver_KS_LCAO::afterscf(const int iter, bool conv)
{
    if (conv || iter==GlobalV::SCF_NMAX)
    {
        //--------------------------------------
        // 1. output charge density for converged,
        // 0 means don't need to consider iter,
        //--------------------------------------
        if( GlobalC::chi0_hilbert.epsilon)                                    // pengfei 2016-11-23
        {
            std::cout <<"eta = "<<GlobalC::chi0_hilbert.eta<<std::endl;
            std::cout <<"domega = "<<GlobalC::chi0_hilbert.domega<<std::endl;
            std::cout <<"nomega = "<<GlobalC::chi0_hilbert.nomega<<std::endl;
            std::cout <<"dim = "<<GlobalC::chi0_hilbert.dim<<std::endl;
            //std::cout <<"oband = "<<GlobalC::chi0_hilbert.oband<<std::endl;
            GlobalC::chi0_hilbert.wfc_k_grid = this->LOWF.wfc_k_grid;
            GlobalC::chi0_hilbert.Chi();
        }

        for(int is=0; is<GlobalV::NSPIN; is++)
        {
            const int precision = 3;

            std::stringstream ssc;
            ssc << GlobalV::global_out_dir << "SPIN" << is + 1 << "_CHG";
            GlobalC::CHR.write_rho(GlobalC::CHR.rho_save[is], is, 0, ssc.str() );//mohan add 2007-10-17

            std::stringstream ssd;
            if(GlobalV::GAMMA_ONLY_LOCAL)
            {
                ssd << GlobalV::global_out_dir << "SPIN" << is + 1 << "_DM";
            }
            else
            {
                ssd << GlobalV::global_out_dir << "SPIN" << is + 1 << "_DM_R";
            }
            this->LOC.write_dm( is, 0, ssd.str(), precision );

            if(GlobalC::pot.out_pot == 1) //LiuXh add 20200701
            {
                std::stringstream ssp;
                ssp << GlobalV::global_out_dir << "SPIN" << is + 1 << "_POT";
                GlobalC::pot.write_potential( is, 0, ssp.str(), GlobalC::pot.vr_eff, precision );
            }

            //LiuXh modify 20200701
            /*
            //fuxiang add 2017-03-15
            std::stringstream sse;
            sse << GlobalV::global_out_dir << "SPIN" << is + 1 << "_DIPOLE_ELEC";
            GlobalC::CHR.write_rho_dipole(GlobalC::CHR.rho_save, is, 0, sse.str());
            */
        }

        if(conv)
        {
            GlobalV::ofs_running << "\n charge density convergence is achieved" << std::endl;
            GlobalV::ofs_running << " final etot is " << GlobalC::en.etot * ModuleBase::Ry_to_eV << " eV" << std::endl;
        }

        if(GlobalV::OUT_LEVEL != "m") 
        {
            Threshold_Elec::print_eigenvalue(GlobalV::ofs_running);
        }

        if(conv)
        {
            //xiaohui add "OUT_LEVEL", 2015-09-16
            if(GlobalV::OUT_LEVEL != "m") GlobalV::ofs_running << std::setprecision(16);
            if(GlobalV::OUT_LEVEL != "m") GlobalV::ofs_running << " EFERMI = " << GlobalC::en.ef * ModuleBase::Ry_to_eV << " eV" << std::endl;
            if(GlobalV::OUT_LEVEL=="ie")
            {
                GlobalV::ofs_running << " " << GlobalV::global_out_dir << " final etot is " << GlobalC::en.etot * ModuleBase::Ry_to_eV << " eV" << std::endl;
            }
        }
        else
        {
            GlobalV::ofs_running << " !! convergence has not been achieved @_@" << std::endl;
            if(GlobalV::OUT_LEVEL=="ie" || GlobalV::OUT_LEVEL=="m") //xiaohui add "m" option, 2015-09-16
            std::cout << " !! CONVERGENCE HAS NOT BEEN ACHIEVED !!" << std::endl;
        }

#ifdef __DEEPKS
        // 2. calculating deepks correction to bandgap
        //and save the results
        if (GlobalV::deepks_bandgap)
        {
            int nocc = GlobalC::CHR.nelec/2;
            ModuleBase::matrix wg_hl;
            wg_hl.create(GlobalV::NSPIN, GlobalV::NBANDS);

            for(int is=0; is<GlobalV::NSPIN; is++)
            {
                for(int ib=0; ib<GlobalV::NBANDS; ib++)
                {
                    wg_hl(is,ib) = 0.0;
                
                    if(ib == nocc-1)
                        wg_hl(is,ib) = -1.0;
                    else if(ib == nocc)
                        wg_hl(is,ib) = 1.0;
                }
            }

            std::vector<ModuleBase::matrix> dm_bandgap_gamma;
            std::vector<ModuleBase::ComplexMatrix> dm_bandgap_k;
        
            if(GlobalV::GAMMA_ONLY_LOCAL)
            {
                dm_bandgap_gamma.resize(GlobalV::NSPIN);
                this->LOC.cal_dm(wg_hl, this->LOWF.wfc_gamma, dm_bandgap_gamma);
                GlobalC::ld.cal_o_delta(dm_bandgap_gamma, *this->LOWF.ParaV);
            }			
            else
            {
                dm_bandgap_k.resize(GlobalC::kv.nks);
                this->LOC.cal_dm(wg_hl, this->LOWF.wfc_k, dm_bandgap_k);
                GlobalC::ld.cal_o_delta_k(dm_bandgap_k, *this->LOWF.ParaV, GlobalC::kv.nks);
            }
            if(GlobalV::deepks_out_labels)
            {
                GlobalC::ld.save_npy_o(GlobalC::wf.ekb[0][nocc] - GlobalC::wf.ekb[0][nocc-1] - GlobalC::ld.o_delta, "o_base.npy");
                GlobalC::ld.cal_orbital_precalc(dm_bandgap_gamma,
                    GlobalC::ucell.nat,
                    GlobalC::ucell,
                    GlobalC::ORB,
                    GlobalC::GridD,
                    *this->LOWF.ParaV);
                GlobalC::ld.save_npy_orbital_precalc(GlobalC::ucell.nat);
            }	
        }

        if (GlobalV::deepks_out_labels)	//caoyu add 2021-06-04
        {
            int nocc = GlobalC::CHR.nelec/2;
            GlobalC::ld.save_npy_o(GlobalC::wf.ekb[0][nocc] - GlobalC::wf.ekb[0][nocc-1], "o_tot.npy");
            if (!GlobalV::deepks_bandgap)
            {
                GlobalC::ld.save_npy_o(GlobalC::wf.ekb[0][nocc] - GlobalC::wf.ekb[0][nocc-1], "o_base.npy");  // no scf, o_tot=o_base	
            }

            GlobalC::ld.save_npy_e(GlobalC::en.etot, "e_tot.npy");
            if (GlobalV::deepks_scf)
            {
                GlobalC::ld.save_npy_e(GlobalC::en.etot - GlobalC::ld.E_delta, "e_base.npy");//ebase :no deepks E_delta including
            }
            else
            {
                GlobalC::ld.save_npy_e(GlobalC::en.etot, "e_base.npy");  // no scf, e_tot=e_base
                                        
            }
        }
#endif

    }
    
    //3. DeePKS PDM and descriptor 
    #ifdef __DEEPKS
    const Parallel_Orbitals* pv = this->LOWF.ParaV;
    if (GlobalV::deepks_out_labels || GlobalV::deepks_scf)
    {
        //this part is for integrated test of deepks
        //so it is printed no matter even if deepks_out_labels is not used
        if(GlobalV::GAMMA_ONLY_LOCAL)
        {
            GlobalC::ld.cal_projected_DM(this->LOC.dm_gamma[0],
                GlobalC::ucell,
                GlobalC::ORB,
                GlobalC::GridD,
                pv->trace_loc_row,
                pv->trace_loc_col);
        }
        else
        {
            GlobalC::ld.cal_projected_DM_k(this->LOC.dm_k,
                GlobalC::ucell,
                GlobalC::ORB,
                GlobalC::GridD,
                pv->trace_loc_row,
                pv->trace_loc_col,
                GlobalC::kv.nks,
                GlobalC::kv.kvec_d);
        }
        GlobalC::ld.cal_descriptor();    //final descriptor
        GlobalC::ld.check_descriptor(GlobalC::ucell);
        
        if (GlobalV::deepks_out_labels) GlobalC::ld.save_npy_d(GlobalC::ucell.nat);            //libnpy needed
    }

    if (GlobalV::deepks_scf)
    {
        if(GlobalV::GAMMA_ONLY_LOCAL)
        {
            GlobalC::ld.cal_e_delta_band(this->LOC.dm_gamma,
                pv->trace_loc_row,
                pv->trace_loc_col,
                pv->nrow);
        }
        else
        {
            GlobalC::ld.cal_e_delta_band_k(this->LOC.dm_k,
            pv->trace_loc_row,
            pv->trace_loc_col,
            GlobalC::kv.nks,
            pv->nrow,
            pv->ncol);
        }
        std::cout << "E_delta_band = " << std::setprecision(8) << GlobalC::ld.e_delta_band << " Ry" << " = " << std::setprecision(8) << GlobalC::ld.e_delta_band * ModuleBase::Ry_to_eV << " eV" << std::endl;
        std::cout << "E_delta_NN= "<<std::setprecision(8) << GlobalC::ld.E_delta << " Ry" << " = "<<std::setprecision(8)<<GlobalC::ld.E_delta*ModuleBase::Ry_to_eV<<" eV"<<std::endl;
    }
#endif
    //4. some outputs
    if(INPUT.dft_plus_dmft)
    {
    // Output sparse overlap matrix S(R)
    this->output_SR("outputs_to_DMFT/overlap_matrix/SR.csr");
    
    // Output wave functions, bands, k-points information, and etc.
    GlobalC::dmft.out_to_dmft(this->LOWF, *this->UHM.LM);
    }

    if(Pdiag_Double::out_mat_hsR)
    {
        this->output_HS_R(); //LiuXh add 2019-07-15
    }

}

}