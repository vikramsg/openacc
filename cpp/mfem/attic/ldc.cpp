#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include <algorithm>

using namespace std;
using namespace mfem;

//Constants
const double gamm  = 1.4;
const double   mu  = 0.0100;
const double R_gas = 287;
const double   Pr  = 0.72;

int problem;

// Velocity coefficient
void init_function(const Vector &x, Vector &v);

// Characteristic boundary condition specification
void char_bnd_cnd(const Vector &x, Vector &v);
// Wall boundary condition specification
void wall_bnd_cnd(const Vector &x, Vector &v);
void lid_bnd_cnd(const Vector &x, Vector &v);

void lid_flux_bnd_cnd(const Vector &x, Vector &v);
void wall_flux_bnd_cnd(const Vector &x, Vector &v);

void getInvFlux(int dim, const Vector &u, Vector &f);

void getVisFlux(int dim, const Vector &u, const Vector &aux_grad, Vector &f);

void getAuxGrad(int dim, const SparseMatrix &K_x, const SparseMatrix &K_y, const SparseMatrix &M, 
        const Vector &u, const Vector &b_aux_x, const Vector &b_aux_y, Vector &aux_grad);
void getAuxVar(int dim, const Vector &u, Vector &aux_sol);

void getFields(const GridFunction &u_sol, const Vector &aux_grad, Vector &rho, Vector &u1, Vector &u2, 
                Vector &E, Vector &T_x, Vector &T_y, Vector &u_x, Vector &u_y, Vector &v_x, Vector &v_y);

/** A time-dependent operator for the right-hand side of the ODE. The DG weak
    form is M du/dt = K u + b, where M and K are the mass
    and operator matrices, and b describes the face correction terms. This can
    be written as a general ODE, du/dt = M^{-1} (K u + b), and this class is
    used to evaluate the right-hand side. */
class FE_Evolution : public TimeDependentOperator
{
private:
   SparseMatrix &M, &K_inv_x, &K_inv_y, &K_vis_x, &K_vis_y;
   const Vector &b;
   const Vector &b_aux_x, &b_aux_y;
   DSmoother M_prec;
   CGSolver M_solver;

public:
   FE_Evolution(SparseMatrix &_M, SparseMatrix &_K_inv_x, SparseMatrix &_K_inv_y, 
                    SparseMatrix &_K_vis_x, SparseMatrix &_K_vis_y, const Vector &_b
                     , const Vector &_b_aux_x, const Vector &_b_aux_y);

   virtual void Mult(const Vector &x, Vector &y) const;

   virtual ~FE_Evolution() { }
};





int main(int argc, char *argv[])
{
   const char *mesh_file = "ldc.msh";
   int    order      = 2;
   double t_final    =20.0000;
   double dt         = 0.0002;
   int    vis_steps  = 400;
   int    ref_levels = 1;

          problem    = 0;    

   int precision = 8;
   cout.precision(precision);

   // 2. Read the mesh from the given mesh file. We can handle geometrically
   //    periodic meshes in this code.
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   int     dim = mesh->Dimension();
   int var_dim = dim + 2;
   int aux_dim = dim + 1; //Auxiliary variables for the viscous terms

   for (int lev = 0; lev < ref_levels; lev++)
   {
      mesh->UniformRefinement();
   }

   // 5. Define the discontinuous DG finite element space of the given
   //    polynomial order on the refined mesh.
   DG_FECollection fec(order, dim);
   FiniteElementSpace fes(mesh, &fec, var_dim);

   cout << "Number of unknowns: " << fes.GetVSize() << endl;

   VectorFunctionCoefficient u0(var_dim, init_function);
   GridFunction u_sol(&fes);
   u_sol.ProjectCoefficient(u0);

   FiniteElementSpace fes_vec(mesh, &fec, dim*var_dim);
   GridFunction f_inv(&fes_vec);
   getInvFlux(dim, u_sol, f_inv);


   ///////////////////////////////////////////////////////////
   // Setup bilinear form for x derivative and the mass matrix
   Vector dir(dim);
   dir(0) = 1.0; dir(1) = 0.0;
   VectorConstantCoefficient x_dir(dir);
   dir(0) = 0.0; dir(1) = 1.0;
   VectorConstantCoefficient y_dir(dir);

   FiniteElementSpace fes_op(mesh, &fec);
   BilinearForm m(&fes_op);
   m.AddDomainIntegrator(new MassIntegrator);
   BilinearForm k_inv_x(&fes_op);
   k_inv_x.AddDomainIntegrator(new ConvectionIntegrator(x_dir, -1.0));
   BilinearForm k_inv_y(&fes_op);
   k_inv_y.AddDomainIntegrator(new ConvectionIntegrator(y_dir, -1.0));

   m.Assemble();
   m.Finalize();
   int skip_zeros = 1;
   k_inv_x.Assemble(skip_zeros);
   k_inv_x.Finalize(skip_zeros);
   k_inv_y.Assemble(skip_zeros);
   k_inv_y.Finalize(skip_zeros);
   SparseMatrix &M   = m.SpMat();
   /////////////////////////////////////////////////////////////
   BilinearForm k_vis_x(&fes_op);
   k_vis_x.AddDomainIntegrator(new ConvectionIntegrator(x_dir, -1.0));
   k_vis_x.AddInteriorFaceIntegrator(
      new TransposeIntegrator(new DGTraceIntegrator(x_dir, 1.0,  0.0)));// Beta 0 means central flux
   BilinearForm k_vis_y(&fes_op);
   k_vis_y.AddDomainIntegrator(new ConvectionIntegrator(y_dir, -1.0));
   k_vis_y.AddInteriorFaceIntegrator(
      new TransposeIntegrator(new DGTraceIntegrator(y_dir, 1.0,  0.0)));// Beta 0 means central flux

   k_vis_x.Assemble(skip_zeros);
   k_vis_x.Finalize(skip_zeros);
   k_vis_y.Assemble(skip_zeros);
   k_vis_y.Finalize(skip_zeros);

   SparseMatrix &K_vis_x = k_vis_x.SpMat();
   SparseMatrix &K_vis_y = k_vis_y.SpMat();
   
   /////////////////////////////////////////////////////////////

   VectorGridFunctionCoefficient u_vec(&u_sol);
   VectorGridFunctionCoefficient f_vec(&f_inv);

   /////////////////////////////////////////////////////////////
   // Linear form
   LinearForm b(&fes);
   b.AddFaceIntegrator(
      new DGEulerIntegrator(u_vec, f_vec, var_dim, -1.0));
   b.Assemble();
   ///////////////////////////////////////////////////////////

   
   FiniteElementSpace fes_aux(mesh, &fec, aux_dim);
   GridFunction aux_var(&fes_aux);
   getAuxVar(dim, u_sol, aux_var);
   VectorGridFunctionCoefficient aux_vec(&aux_var);

   ///////////////////////////////////////////////////////////
   // Linear form for boundary condition
   // Each boundary should get its own array since they are passed by reference
   Array<int> dir_bdr_1(mesh->bdr_attributes.Max());

   VectorFunctionCoefficient u_lid_bnd(aux_dim, lid_bnd_cnd); // Defines lid boundary condition
   VectorFunctionCoefficient u_lid_flux_bnd(var_dim, lid_flux_bnd_cnd); // Defines lid boundary condition
   // Linear form for boundary condition
   dir_bdr_1    = 0; // Deactivate all boundaries
   dir_bdr_1[0] = 1; // Activate lid boundary 

   LinearForm b1(&fes);
   b1.AddBdrFaceIntegrator(
      new DG_Euler_NoSlip_Integrator(
      u_vec, f_vec, u_lid_bnd, -1.0), dir_bdr_1); 
   b1.Assemble();

   LinearForm b_aux_x_1(&fes_aux);
   b_aux_x_1.AddBdrFaceIntegrator(
      new DG_CNS_Aux_Integrator(
      x_dir, aux_vec, u_lid_bnd, 1.0), dir_bdr_1); 
   b_aux_x_1.Assemble();

   LinearForm b_aux_y_1(&fes_aux);
   b_aux_y_1.AddBdrFaceIntegrator(
      new DG_CNS_Aux_Integrator(
      y_dir, aux_vec, u_lid_bnd,  1.0), dir_bdr_1); 
   b_aux_y_1.Assemble();
   ///////////////////////////////////////////////////////////

   Array<int> dir_bdr_2(mesh->bdr_attributes.Max());

   VectorFunctionCoefficient u_wall_bnd(aux_dim, wall_bnd_cnd); // Defines wall boundary condition
   VectorFunctionCoefficient u_wall_flux_bnd(var_dim, wall_flux_bnd_cnd); // Defines wall boundary condition
   // Linear form for boundary condition
   dir_bdr_2    = 0; // Deactivate all boundaries
   dir_bdr_2[1] = 1; // Activate lid boundary 

   LinearForm b2(&fes);
   b2.AddBdrFaceIntegrator(
      new DG_Euler_NoSlip_Integrator(
      u_vec, f_vec, u_wall_bnd, -1.0), dir_bdr_2); 
   b2.Assemble();

   LinearForm b_aux_x_2(&fes_aux);
   b_aux_x_2.AddBdrFaceIntegrator(
      new DG_CNS_Aux_Integrator(
      x_dir, aux_vec, u_wall_bnd, 1.0), dir_bdr_2); 
   b_aux_x_2.Assemble();

   LinearForm b_aux_y_2(&fes_aux);
   b_aux_y_2.AddBdrFaceIntegrator(
      new DG_CNS_Aux_Integrator(
      y_dir, aux_vec, u_wall_bnd,  1.0), dir_bdr_2); 
   b_aux_y_2.Assemble();
   ///////////////////////////////////////////////////////////

   LinearForm b_aux_x(&fes_aux);
   LinearForm b_aux_y(&fes_aux);

   add(b_aux_x_1, b_aux_x_2, b_aux_x);
   add(b_aux_y_1, b_aux_y_2, b_aux_y);

   FiniteElementSpace fes_aux_grad(mesh, &fec, dim*aux_dim);
   GridFunction aux_grad(&fes_aux_grad);
   getAuxGrad(dim, K_vis_x, K_vis_y, M, u_sol, b_aux_x, b_aux_y, aux_grad);
   GridFunction f_vis(&fes_vec);
   getVisFlux(dim, u_sol, aux_grad, f_vis);

   ///////////////////////////////////////////////////////////
   //
   //
   // Create data collection for solution output: either VisItDataCollection for
   // ascii data files, or SidreDataCollection for binary data files.
   DataCollection *dc = NULL;
   
   dc = new VisItDataCollection("Euler", mesh);
   dc->SetPrecision(precision);
   
   FiniteElementSpace fes_fields(mesh, &fec);
   GridFunction rho(&fes_fields);
   GridFunction u1(&fes_fields);
   GridFunction u2(&fes_fields);
   GridFunction E(&fes_fields);
   GridFunction T_x(&fes_fields);
   GridFunction T_y(&fes_fields);
   GridFunction u_x(&fes_fields);
   GridFunction u_y(&fes_fields);
   GridFunction v_x(&fes_fields);
   GridFunction v_y(&fes_fields);

   dc->RegisterField("rho", &rho);
   dc->RegisterField("u1", &u1);
   dc->RegisterField("u2", &u2);
   dc->RegisterField("E", &E);
   dc->RegisterField("T_x", &T_x);
   dc->RegisterField("T_y", &T_y);
   dc->RegisterField("u_x", &u_x);
   dc->RegisterField("u_y", &u_y);
   dc->RegisterField("v_x", &v_x);
   dc->RegisterField("v_y", &v_y);

   getFields(u_sol, aux_grad, rho, u1, u2, E, T_x, T_y, u_x, u_y, v_x, v_y);

   dc->SetCycle(0);
   dc->SetTime(0.0);
   dc->Save();


   FE_Evolution adv(m.SpMat(), k_inv_x.SpMat(), k_inv_y.SpMat(), 
                        K_vis_x, K_vis_y, b, b_aux_x, b_aux_y);
//   ODESolver *ode_solver = new ForwardEulerSolver; 
   ODESolver *ode_solver = new RK3SSPSolver; 


   double t = 0.0;
   adv.SetTime(t);
   ode_solver->Init(adv);

   bool done = false;
   for (int ti = 0; !done; )
   {
      b.Assemble();
      b1.Assemble();
      b2.Assemble();

      add(b, b1, b);
      add(b, b2, b);

      b_aux_x_1.Assemble();
      b_aux_y_1.Assemble();
      b_aux_x_2.Assemble();
      b_aux_y_2.Assemble();

      add(b_aux_x_1, b_aux_x_2, b_aux_x);
      add(b_aux_y_1, b_aux_y_2, b_aux_y);

      double dt_real = min(dt, t_final - t);
      ode_solver->Step(u_sol, t, dt_real);
      ti++;

      cout << "time step: " << ti << ", time: " << t << ", max_sol: " << u_sol.Max() << endl;

      getInvFlux(dim, u_sol, f_inv); // To update f_vec

      getAuxVar(dim, u_sol, aux_var);

      done = (t >= t_final - 1e-8*dt);

      if (done || ti % vis_steps == 0)
      {
          getAuxGrad(dim, K_vis_x, K_vis_y, M, u_sol, b_aux_x, b_aux_y, aux_grad);
          getFields(u_sol, aux_grad, rho, u1, u2, E, T_x, T_y, u_x, u_y, v_x, v_y);

          dc->SetCycle(ti);
          dc->SetTime(t);
          dc->Save();
      }
   }


   // Print all nodes in the finite element space 
   FiniteElementSpace fes_nodes(mesh, &fec, dim);
   GridFunction nodes(&fes_nodes);
   mesh->GetNodes(nodes);

   for (int i = 0; i < nodes.Size()/dim; i++)
   {
       int offset = nodes.Size()/dim;
       int sub1 = i, sub2 = offset + i, sub3 = 2*offset + i, sub4 = 3*offset + i;
//       cout << nodes(sub1) << '\t' << nodes(sub2) << '\t' << u_sol(sub3) << '\t' << aux_grad[4*offset + i]<< endl;      
//       cout << nodes(sub1) << '\t' << nodes(sub2) << '\t' << u_sol(sub1) << '\t' << b_aux_y_1[sub1]<< endl;      
//       cout << nodes(sub1) << '\t' << nodes(sub2) << '\t' << u_sol(sub1) << '\t' << b[sub3] << "\t" << b1[sub1] << "\t" << b2[sub4]<< endl;      
//       cout << nodes(sub1) << '\t' << nodes(sub2) << '\t' << u_sol(sub4) << '\t' << aux_var(sub3) << "\t" << thermal_k*aux_grad(sub3) << endl;      
   }

   return 0;
}


// Implementation of class FE_Evolution
FE_Evolution::FE_Evolution(SparseMatrix &_M, SparseMatrix &_K_inv_x, SparseMatrix &_K_inv_y, 
           SparseMatrix &_K_vis_x, SparseMatrix &_K_vis_y, const Vector &_b
            ,const Vector &_b_aux_x, const Vector &_b_aux_y) 
   : TimeDependentOperator(_b.Size()), M(_M), K_inv_x(_K_inv_x), K_inv_y(_K_inv_y), K_vis_x(_K_vis_x), 
        K_vis_y(_K_vis_y), b(_b), b_aux_x(_b_aux_x), b_aux_y(_b_aux_y)
{
   M_solver.SetPreconditioner(M_prec);
   M_solver.SetOperator(M);

   M_solver.iterative_mode = false;
   M_solver.SetRelTol(1e-9);
   M_solver.SetAbsTol(0.0);
   M_solver.SetMaxIter(100);
   M_solver.SetPrintLevel(0);
}



void FE_Evolution::Mult(const Vector &x, Vector &y) const
{
    int dim = x.Size()/K_inv_x.Size() - 2;
    int var_dim = dim + 2;

    y.SetSize(x.Size()); // Needed since by default ode_init will set it as size of K

    Vector y_temp;
    y_temp.SetSize(x.Size()); // Needed since by default ode_init will set it as size of K

    Vector f(dim*x.Size());
    getInvFlux(dim, x, f);

    int offset  = K_inv_x.Size();
    Array<int> offsets[dim*var_dim];
    for(int i = 0; i < dim*var_dim; i++)
    {
        offsets[i].SetSize(offset);
    }

    for(int j = 0; j < dim*var_dim; j++)
    {
        for(int i = 0; i < offset; i++)
        {
            offsets[j][i] = j*offset + i ;
        }
    }

    Vector f_sol(offset), f_x(offset), f_x_m(offset);
    Vector b_sub(offset);
    y = 0.0;
    for(int i = 0; i < var_dim; i++)
    {
        f.GetSubVector(offsets[i], f_sol);
        K_inv_x.Mult(f_sol, f_x);
        b.GetSubVector(offsets[i], b_sub);
        f_x += b_sub; // Needs to be added only once
        M_solver.Mult(f_x, f_x_m);
        y_temp.SetSubVector(offsets[i], f_x_m);
    }
    y += y_temp;
    for(int i = var_dim + 0; i < 2*var_dim; i++)
    {
        f.GetSubVector(offsets[i], f_sol);
        K_inv_y.Mult(f_sol, f_x);
        M_solver.Mult(f_x, f_x_m);
        y_temp.SetSubVector(offsets[i - var_dim], f_x_m);
    }
    y += y_temp;

    //////////////////////////////////////////////
    //Get viscous contribution
    Vector f_vis(dim*x.Size());
    Vector aux_grad(dim*(dim + 1)*offset);
    getAuxGrad(dim, K_vis_x, K_vis_y, M, x, b_aux_x, b_aux_y, aux_grad);
    getVisFlux(dim, x, aux_grad, f_vis);

    for(int i = 0; i < var_dim; i++)
    {
        f_vis.GetSubVector(offsets[i], f_sol);
        K_vis_x.Mult(f_sol, f_x);
        f_x *= -1; // rhs = +f_v
        M_solver.Mult(f_x, f_x_m);
        y_temp.SetSubVector(offsets[i], f_x_m);
    }

    y += y_temp;

    for(int i = var_dim + 0; i < 2*var_dim; i++)
    {
        f_vis.GetSubVector(offsets[i], f_sol);
        K_vis_y.Mult(f_sol, f_x);
        f_x *= -1;
        M_solver.Mult(f_x, f_x_m);
        y_temp.SetSubVector(offsets[i - var_dim], f_x_m);
    }
    y += y_temp;

//    for (int j = 0; j < offset; j++) 
//    {
//        int sub = 1*offset + j ;
//        cout << j << "\t" << x(j) << '\t'<< f(sub) << "\t" << b(sub) << "\t" << y(sub) << endl;
//    }


//    for (int j = 0; j < offset; j++) cout << j << '\t'<< b(j) << endl;
//    for (int j = 0; j < offset; j++) cout << x(offset + j) << '\t'<< f(var_dim*offset + 3*offset + j) << endl;
//    for (int j = 0; j < offset; j++) cout << x(0*offset + j) << '\t'<< y(0*offset + j) << endl;

}



// Inviscid flux 
void getInvFlux(int dim, const Vector &u, Vector &f)
{
    int var_dim = dim + 2;
    int offset  = u.Size()/var_dim;

    Array<int> offsets[dim*var_dim];
    for(int i = 0; i < dim*var_dim; i++)
    {
        offsets[i].SetSize(offset);
    }

    for(int j = 0; j < dim*var_dim; j++)
    {
        for(int i = 0; i < offset; i++)
        {
            offsets[j][i] = j*offset + i ;
        }
    }
    Vector rho, rho_u1, rho_u2, E;
    u.GetSubVector(offsets[0], rho   );
    u.GetSubVector(offsets[3],      E);

    Vector rho_vel[dim];
    for(int i = 0; i < dim; i++) u.GetSubVector(offsets[1 + i], rho_vel[i]);

    for(int i = 0; i < offset; i++)
    {
        double vel[dim];        
        for(int j = 0; j < dim; j++) vel[j]   = rho_vel[j](i)/rho(i);

        double vel_sq = 0.0;
        for(int j = 0; j < dim; j++) vel_sq += pow(vel[j], 2);

        double pres    = (E(i) - 0.5*rho(i)*vel_sq)*(gamm - 1);

        for(int j = 0; j < dim; j++) 
        {
            f(j*var_dim*offset + i)       = rho_vel[j][i]; //rho*u

            for (int k = 0; k < dim ; k++)
            {
                f(j*var_dim*offset + (k + 1)*offset + i)     = rho_vel[j](i)*vel[k]; //rho*u*u + p    
            }
            f(j*var_dim*offset + (j + 1)*offset + i)        += pres; 

            f(j*var_dim*offset + (var_dim - 1)*offset + i)   = (E(i) + pres)*vel[j] ;//(E+p)*u
        }
    }
}


// Get gradient of auxilliary variable 
void getAuxGrad(int dim, const SparseMatrix &K_x, const SparseMatrix &K_y, const SparseMatrix &M, 
        const Vector &u, const Vector &b_aux_x, const Vector &b_aux_y, Vector &aux_grad)
{
    CGSolver M_solver;

    M_solver.SetOperator(M);
    M_solver.iterative_mode = false;

    int var_dim = dim + 2; 
    int aux_dim = dim + 1; // Auxilliary variables are {u, v, w, T}
    int offset = u.Size()/var_dim;

    Vector aux_sol(aux_dim*offset);

    getAuxVar(dim, u, aux_sol);
        
    Array<int> offsets[dim*aux_dim];
    for(int i = 0; i < dim*aux_dim; i++)
    {
        offsets[i].SetSize(offset);
    }
    for(int j = 0; j < dim*aux_dim; j++)
    {
        for(int i = 0; i < offset; i++)
        {
            offsets[j][i] = j*offset + i ;
        }
    }

    aux_grad = 0.0;

    Vector aux_var(offset), aux_x(offset), b_sub(offset);
    for(int i = 0; i < aux_dim; i++)
    {
        aux_sol.GetSubVector(offsets[i], aux_var);

        K_x.Mult(aux_var, aux_x);
        aux_x *= -1.0; // K_vis = -d/dx 
        b_aux_x.GetSubVector(offsets[i], b_sub);
        add(b_sub, aux_x, aux_x);
        M_solver.Mult(aux_x, aux_x);
        aux_grad.SetSubVector(offsets[          i], aux_x);

        K_y.Mult(aux_var, aux_x);
        aux_x *= -1.0; // K_vis = -d/dx 
        b_aux_y.GetSubVector(offsets[i], b_sub);
        add(b_sub, aux_x, aux_x);
        M_solver.Mult(aux_x, aux_x);
        aux_grad.SetSubVector(offsets[aux_dim + i], aux_x);
    }
}



// Get auxilliary variables
void getAuxVar(int dim, const Vector &u, Vector &aux_sol)
{
    int var_dim = dim + 2; 
    int aux_dim = dim + 1; // Auxilliary variables are {u, v, w, T}

    int offset = u.Size()/var_dim;

    Array<int> offsets[var_dim];
    for(int i = 0; i < var_dim; i++)
    {
        offsets[i].SetSize(offset);
    }
    for(int j = 0; j < var_dim; j++)
    {
        for(int i = 0; i < offset; i++)
        {
            offsets[j][i] = j*offset + i ;
        }
    }

    Vector rho(offset), u_sol(offset), rho_E(offset);
    u.GetSubVector(offsets[0], rho);
    for(int i = 0; i < dim; i++)
    {
        u.GetSubVector(offsets[1 + i], u_sol); // u, v, w
        for(int j = 0; j < offset; j++)
        {
            aux_sol[i*offset + j] = u_sol[j]/rho[j];
        }
    }
    u.GetSubVector(offsets[var_dim - 1], rho_E); // rho*E
    for(int j = 0; j < offset; j++)
    {
        double v_sq =  0;
        for(int i = 0; i < dim; i++)
        {
            v_sq += pow(aux_sol[i*offset + j], 2);
        }
        double e  = (rho_E(j) - 0.5*rho[j]*v_sq)/rho[j];
        double Cv = R_gas/(gamm - 1);

        aux_sol[(aux_dim - 1)*offset + j] = e/Cv; // T
    }
}


// Aux flux 
void getVisFlux(int dim, const Vector &u, const Vector &aux_grad, Vector &f)
{
    int var_dim = dim + 2;
    int aux_dim = dim + 1;
    int offset  = u.Size()/var_dim;

    Array<int> offsets[dim*var_dim];
    for(int i = 0; i < dim*var_dim; i++)
    {
        offsets[i].SetSize(offset);
    }

    for(int j = 0; j < dim*var_dim; j++)
    {
        for(int i = 0; i < offset; i++)
        {
            offsets[j][i] = j*offset + i ;
        }
    }

    Vector rho(offset), rho_u1(offset), rho_u2(offset), E(offset);
    u.GetSubVector(offsets[0], rho   );
    u.GetSubVector(offsets[3],      E);

    Vector rho_vel[dim];
    for(int i = 0; i < dim; i++) u.GetSubVector(offsets[1 + i], rho_vel[i]);

    for(int i = 0; i < offset; i++)
    {
        double vel[dim];        
        for(int j = 0; j < dim; j++) vel[j]   = rho_vel[j](i)/rho(i);

        double vel_grad[dim][dim];
        for (int k = 0; k < dim; k++)
            for (int j = 0; j < dim; j++)
            {
                vel_grad[j][k]      =  aux_grad[k*(aux_dim)*offset + j*offset + i];
            }

        double divergence = 0.0;            
        for (int k = 0; k < dim; k++) divergence += vel_grad[k][k];

        double tau[dim][dim];
        for (int j = 0; j < dim; j++) 
            for (int k = 0; k < dim; k++) 
                tau[j][k] = mu*(vel_grad[j][k] + vel_grad[k][j]);

        for (int j = 0; j < dim; j++) tau[j][j] -= 2.0*mu*divergence/3.0; 

        double int_en_grad[dim];
        for (int j = 0; j < dim; j++)
        {
            int_en_grad[j] = (R_gas/(gamm - 1))*aux_grad[j*(aux_dim)*offset + (aux_dim - 1)*offset + i] ; // Cv*T_x
        }

        for (int j = 0; j < dim ; j++)
        {
            f(j*var_dim*offset + i)       = 0.0;

            for (int k = 0; k < dim ; k++)
            {
                f(j*var_dim*offset + (k + 1)*offset + i)       = tau[j][k];
            }
            f(j*var_dim*offset + (var_dim - 1)*offset + i)     =  (mu/Pr)*gamm*int_en_grad[j]; 
            for (int k = 0; k < dim ; k++)
            {
                f(j*var_dim*offset + (var_dim - 1)*offset + i)+= vel[k]*tau[j][k]; 
            }
        }
//        cout << i << "\t" << f(2*offset + i) << endl;
    }
}




//  Initialize variables coefficient
void init_function(const Vector &x, Vector &v)
{
   //Space dimensions 
   int dim = x.Size();

   if (dim == 2)
   {
       double rho, u1, u2, p;
       if (problem == 0)
       {
           rho = 1;
           u1 = 0.0; u2 = 0.0;
           p   = 100;
       }
       else if (problem == 1)
       {
           if (x(0) < 0.0)
           {
               rho = 1.0; 
               u1  = 0.0; u2 = 0.0;
               p   = 1;
           }
           else
           {
               rho = 0.125;
               u1  = 0.0; u2 = 0.0;
               p   = 0.1;
           }
       }
       else if (problem == 2) //Taylor Green Vortex
       {
           rho =  1.0;
           p   =  100 + rho/4.0*(cos(2.0*M_PI*x(0)) + cos(2.0*M_PI*x(1)));
           u1  =      sin(M_PI*x(0))*cos(M_PI*x(1))/rho;
           u2  = -1.0*cos(M_PI*x(0))*sin(M_PI*x(1))/rho;
       }

    
       double v_sq = pow(u1, 2) + pow(u2, 2);
    
       v(0) = rho;                     //rho
       v(1) = rho * u1;                //rho * u
       v(2) = rho * u2;                //rho * v
       v(3) = p/(gamm - 1) + 0.5*rho*v_sq;
   }
}

void lid_bnd_cnd(const Vector &x, Vector &v)
{
   //Space dimensions 
   int dim = x.Size();

   if (dim == 2)
   {
       v(0) = 1.0;  // x velocity   
       v(1) = 0.0;  // y velocity 
       v(2) = 0.3484;  // Temperature 
   }
}


void wall_bnd_cnd(const Vector &x, Vector &v)
{
   //Space dimensions 
   int dim = x.Size();

   if (dim == 2)
   {
       v(0) = 0.0;  // x velocity   
       v(1) = 0.0;  // y velocity 
       v(2) = 0.3484;  // Temperature 
   }
}


void getFields(const GridFunction &u_sol, const Vector &aux_grad, Vector &rho, Vector &u1, Vector &u2, 
                Vector &E, Vector &T_x, Vector &T_y, Vector &u_x, Vector &u_y, Vector &v_x, Vector &v_y)
{

    int vDim    = u_sol.VectorDim();
    int aux_dim = vDim - 1;
    int dofs    = u_sol.Size()/vDim;

    for (int i = 0; i < dofs; i++)
    {
        rho[i] = u_sol[         i];        
        u1 [i] = u_sol[  dofs + i]/rho[i];        
        u2 [i] = u_sol[2*dofs + i]/rho[i];        
        E  [i] = u_sol[3*dofs + i];        

        u_x[i] = aux_grad[(0          )*dofs + i];
        u_y[i] = aux_grad[(aux_dim    )*dofs + i];

        v_x[i] = aux_grad[(1          )*dofs + i];
        v_y[i] = aux_grad[(aux_dim + 1)*dofs + i];

        T_x[i] = aux_grad[(aux_dim - 1)*dofs + i];
        T_y[i] = aux_grad[aux_dim*dofs + (aux_dim - 1)*dofs + i];
    }
}
