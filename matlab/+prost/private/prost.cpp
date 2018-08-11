#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "prost/common.hpp"
#include "prost/exception.hpp"
#include "factory.hpp"

using namespace matlab; 
using namespace prost;

// redirect std::cout to mexPrintf
class mexstream : public std::streambuf {
 protected:
  virtual std::streamsize xsputn(const char *s, std::streamsize n) { 
    mexPrintf("%.*s", n, s);

    return n; 
  }

  virtual int overflow(int c = EOF) {
    if (c != EOF) {
      mexPrintf("%.1s", &c);

      if (c == '\n')
        mexEvalString("pause(.001);"); // allows MATLAB to flush output
    }; 
    return 1;
  }
};

class scoped_redirect_cout {
 public:
  scoped_redirect_cout() { old_buf = std::cout.rdbuf(); std::cout.rdbuf(&mout); }
  ~scoped_redirect_cout() { std::cout.rdbuf(old_buf); }

 private:
  mexstream mout;
  std::streambuf *old_buf;
};

scoped_redirect_cout mex_cout_redirect; 

#ifdef __cplusplus
extern "C" bool utIsInterruptPending();
extern "C" void utSetInterruptPending(bool);
#else
extern bool utIsInterruptPending();
extern void utSetInterruptPending(bool);
#endif

#define MEX_ARGS int nlhs, mxArray **plhs, int nrhs, const mxArray **prhs

static int current_gpu_device = 0;

static bool MexStoppingCallback() {
  if(utIsInterruptPending())
  {
    utSetInterruptPending(false);
    return true;
  }
  
  return false;
}

static void SolveProblem(MEX_ARGS) {
  cudaError_t error = cudaSetDevice(current_gpu_device);
  cudaDeviceReset();
  if(error != cudaSuccess)
    throw Exception("Invalid CUDA device.");

  BlockDiags<real>::ResetConstMem();

  size_t nrows = static_cast<size_t>(mxGetScalar(prhs[1]));
  size_t ncols = static_cast<size_t>(mxGetScalar(prhs[2]));

  std::shared_ptr<Problem<real> > problem = CreateProblem(prhs[0], nrows, ncols);
  std::shared_ptr<Backend<real> > backend = CreateBackend(prhs[3]);
  Solver<real>::Options opts = CreateSolverOptions(prhs[4]);

  if(opts.verbose) {
    std::cout << "prost v" << prost::get_version().c_str() << std::endl;
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, current_gpu_device);
    int sm_per_multiproc = _ConvertSMVer2Cores(prop.major, prop.minor);
    printf("Running on device number %d: %s (%.1f GB, %d Cores), float precision: %d bit.\n", current_gpu_device, prop.name, (double)prop.totalGlobalMem/(1024.*1024*1024), prop.multiProcessorCount * sm_per_multiproc, sizeof(real) * 8);
  }

  std::shared_ptr<Solver<real> > solver( new Solver<real>(problem, backend) );
  solver->SetOptions(opts);
  solver->SetIntermCallback(SolverIntermCallback);
  solver->SetStoppingCallback(MexStoppingCallback);

  solver->Initialize();

  Solver<real>::ConvergenceResult result = solver->Solve();

  // Copy result back to MATLAB
  mxArray *mex_primal_sol = mxCreateDoubleMatrix(problem->ncols(), 1, mxREAL);
  mxArray *mex_primal_constr_sol = mxCreateDoubleMatrix(problem->nrows(), 1, mxREAL);
  mxArray *mex_dual_sol = mxCreateDoubleMatrix(problem->nrows(), 1, mxREAL);
  mxArray *mex_dual_constr_sol = mxCreateDoubleMatrix(problem->ncols(), 1, mxREAL);
  mxArray *result_string;

  switch(result)
  {
    case Solver<real>::ConvergenceResult::kConverged:
      result_string = mxCreateString("Converged.");
      break;

    case Solver<real>::ConvergenceResult::kStoppedMaxIters:
      result_string = mxCreateString("Reached maximum iterations.");
      break;

    case Solver<real>::ConvergenceResult::kStoppedUser:
      result_string = mxCreateString("Stopped by user.");
      break;
  }

  std::copy(solver->cur_dual_sol().begin(),
	    solver->cur_dual_sol().end(),
	    (double *)mxGetPr(mex_dual_sol));

  std::copy(solver->cur_primal_sol().begin(),
	    solver->cur_primal_sol().end(),
	    (double *)mxGetPr(mex_primal_sol));

  std::copy(solver->cur_primal_constr_sol().begin(),
            solver->cur_primal_constr_sol().end(),
            (double *)mxGetPr(mex_primal_constr_sol));

  std::copy(solver->cur_dual_constr_sol().begin(),
            solver->cur_dual_constr_sol().end(),
            (double *)mxGetPr(mex_dual_constr_sol));

  const char *fieldnames[5] = {
    "x",
    "y",
    "z",
    "w",
    "result"
  };

  plhs[0] = mxCreateStructMatrix(1, 1, 5, fieldnames);

  mxSetFieldByNumber(plhs[0], 0, 0, mex_primal_sol);
  mxSetFieldByNumber(plhs[0], 0, 1, mex_dual_sol);
  mxSetFieldByNumber(plhs[0], 0, 2, mex_primal_constr_sol);
  mxSetFieldByNumber(plhs[0], 0, 3, mex_dual_constr_sol);
  mxSetFieldByNumber(plhs[0], 0, 4, result_string);

  solver->Release();
}

static void EvalLinOp(MEX_ARGS) {
  BlockDiags<real>::ResetConstMem();

  if(nrhs != 3)
    throw Exception("eval_lin_op: Three inputs required!");

  if(nlhs < 3)
    throw Exception("eval_lin_op: At least three outputs (result, rowsum, colsum) required.");

  cudaError_t error = cudaSetDevice(current_gpu_device);
  cudaDeviceReset();
  if(error != cudaSuccess)
    throw Exception("Invalid CUDA device.");

  // read input arguments
  std::shared_ptr<LinearOperator<real> > linop(new LinearOperator<real>());

  const mxArray *cell_linop = prhs[0];
  const mwSize *dims_linop = mxGetDimensions(cell_linop);

  for(mwSize i = 0; i < dims_linop[0]; i++)
    linop->AddBlock( std::shared_ptr<Block<real> >(CreateBlock(mxGetCell(cell_linop, i))) );

  std::vector<real> rhs;
  bool transpose = (mxGetScalar(prhs[2]) > 0);
  const mwSize *dims = mxGetDimensions(prhs[1]);

  if(dims[1] != 1)
    throw Exception("Right-hand side input to eval_linop should be a n-times-1 vector!");

  linop->Initialize();

  double *matlab_rhs = (double *)mxGetPr(prhs[1]);

  if(transpose)
    rhs = std::vector<real>( matlab_rhs, matlab_rhs + linop->nrows());
  else
    rhs = std::vector<real>( matlab_rhs, matlab_rhs + linop->ncols());

  std::vector<real> res;

  double time;
  if(transpose)
    time = linop->EvalAdjoint(res, rhs);
  else 
    time = linop->Eval(res, rhs);

  plhs[0] = mxCreateDoubleMatrix(transpose ? linop->ncols() : linop->nrows(), 1, mxREAL);
  plhs[1] = mxCreateDoubleMatrix(linop->nrows(), 1, mxREAL);
  plhs[2] = mxCreateDoubleMatrix(linop->ncols(), 1, mxREAL);

  std::copy(res.begin(), res.end(), (double *)mxGetPr(plhs[0]));
  std::vector<double> rowsum(linop->nrows());
  std::vector<double> colsum(linop->ncols());

  for(size_t row = 0; row < linop->nrows(); row++)
    rowsum[row] = linop->row_sum(row, 1);

  for(size_t col = 0; col < linop->ncols(); col++)
    colsum[col] = linop->col_sum(col, 1);

  std::copy(rowsum.begin(), rowsum.end(), (double *)mxGetPr(plhs[1]));
  std::copy(colsum.begin(), colsum.end(), (double *)mxGetPr(plhs[2]));

  plhs[3] = mxCreateDoubleMatrix(1, 1, mxREAL);
  double *pr = (double *)mxGetPr(plhs[3]);
  pr[0] = time;
}

static void EvalProx(MEX_ARGS) {
  if(nrhs < 4)
    throw Exception("eval_prox: At least four inputs required.");

  if(nlhs == 0)
    throw Exception("One output (result of prox) required.");

  cudaError_t error = cudaSetDevice(current_gpu_device);
  cudaDeviceReset();
  if(error != cudaSuccess)
    throw Exception("Invalid CUDA device.");

  // check dimensions
  const mwSize *dims = mxGetDimensions(prhs[1]);

  size_t n = dims[0];
  if(dims[1] != 1)
    throw Exception("Input to prox should be a vector!");

  // init prox
  std::shared_ptr<Prox<real>> prox;

  prox = CreateProx(prhs[0]);
  prox->Initialize();

  if(prox->size() != n)
  {
    stringstream ss;
    ss << "Size of input argument (" << n << ") doesn't match size of prox (" << prox->size() << ")!\n";
    throw Exception(ss.str());
  }

  // read data from matlab
  double *arg = (double *)mxGetPr(prhs[1]);
  double *tau_diag = (double *)mxGetPr(prhs[3]);

  std::vector<real> h_result;
  std::vector<real> h_arg(arg, arg + n);
  std::vector<real> h_tau(tau_diag, tau_diag + n);
  real tau = (real) mxGetScalar(prhs[2]);

  double milliseconds = prox->Eval(h_result, h_arg, h_tau, tau);

  // convert result back to MATLAB matrix and float -> double
  plhs[0] = mxCreateDoubleMatrix(n, 1, mxREAL);
  std::copy(h_result.begin(), h_result.end(), (double *)mxGetPr(plhs[0]));

  plhs[1] = mxCreateDoubleMatrix(1, 1, mxREAL);
  double *pr = (double *)mxGetPr(plhs[1]);
  pr[0] = milliseconds;
}

static void Init(MEX_ARGS) {
  mexLock(); 
}

static void Release(MEX_ARGS) {
  mexUnlock();
  cudaDeviceReset();
}

static void ListGPUs(MEX_ARGS) {
  int nDevices;

  cudaGetDeviceCount(&nDevices);
  for (int i = 0; i < nDevices; i++) {
    cudaDeviceProp prop;
    cudaGetDeviceProperties(&prop, i);
    int sm_per_multiproc = _ConvertSMVer2Cores(prop.major, prop.minor);
    printf("Device number %d: %s (%.1f GB, %d Cores).\n", i, prop.name, (double)prop.totalGlobalMem/(1024.*1024*1024), prop.multiProcessorCount * sm_per_multiproc);
  }
}

static void SetGPU(MEX_ARGS) {
  int id = mxGetScalar(prhs[0]);

  current_gpu_device = id;
}

const static map<string, function<void(MEX_ARGS)>> cmd_reg = {
  { "init",          Init         },
  { "release",       Release      },
  { "solve_problem", SolveProblem },
  { "eval_linop",    EvalLinOp    },
  { "eval_prox",     EvalProx     },
  { "list_gpus",     ListGPUs     },
  { "set_gpu",       SetGPU       },
};

void mexFunction(MEX_ARGS)
{
  if(nrhs == 0)
    mexErrMsgTxt("Usage: prost_(command, arg1, arg2, ...);");

  char *cmd = mxArrayToString(prhs[0]);
  bool executed = false;
  
  try 
  {
    for(auto& c : cmd_reg)
    {
      if(c.first.compare(cmd) == 0)
      {
        c.second(nlhs, plhs, nrhs - 1, prhs + 1);
        executed = true;
        break;
      }
    }

    if(!executed)
    {
      stringstream msg;
      msg << "Unknown command '" << cmd << "'.";
      throw Exception(msg.str());
    }
  }
  catch(const Exception& e)
  {
    mexErrMsgTxt(e.what());
    Release(nlhs, plhs, nrhs - 1, prhs + 1);
  }
} 
