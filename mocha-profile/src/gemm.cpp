// main profiling tool.
#include <iostream>
#include <ctime>
#include <limits>
#include <cmath>
#include <memory>
#include <fstream>
#include <unistd.h>

#include <CL/cl.h>

#include "cmdoptions.hpp"
#include "energy.hpp"

using namespace std;

// global variables.
std::shared_ptr<CmdParserMochaGEMM> cmdparser;
int clGlobalSize = 1024, clLocalSize = 16;

size_t size_a, size_b, size_c;

// import modules to be profiled.
// modules have access to our global variables.
#include "gemm-gpu.cpp"
#include "gemm-cpu.cpp"
#include "gemm-viennacl.cpp"

template <class T>
bool checkValidity (
    const T* A,     // left input matrix, column-major or row-major depending on Atransposed argument
    const T* B,     // right input matrix, column-major or row-major depending on Btransposed argument
    const T* C,     // output matrix, column-major
    size_t size_a,    
    size_t size_b,  
    size_t size_c,
    bool A_row_major = true,
    bool B_row_major = true,
    bool C_row_major = true
)
{
    cout << "Validate output..." << flush;

    size_t listride = A_row_major ? 1 : size_a;
    size_t istride = A_row_major ? size_b : 1;

    size_t ljstride = B_row_major ? size_c : 1;
    size_t jstride = B_row_major ? 1 : size_b;

    // that initial matrix values are from [0, 1]
    T max_value = 1;
    // T error_tol = T(2) * max_value * max_value * T(2) * size * numeric_limits<T>::epsilon();
    T error_tol = T(1e-4);

    for(size_t i = 0; i < size_a; ++i)
    {
        for(size_t j = 0; j < size_c; ++j)
        {
            // compute golden value for c[i][j] element
            T accum = 0;
            for(size_t l = 0; l < size_b; ++l)
            {
                accum += A[l*listride + i * istride] * B[l * ljstride + j * jstride];
            }

            T golden = accum;

            T absdiff, C_entry;
            if(C_row_major) { 
              C_entry = C[i * size_c + j];
            }else{
              C_entry = C[j * size_b + i];
            }
            absdiff = abs(C_entry - golden);

            if(absdiff / golden > error_tol)
            {
                cout << " FAILED\n";
                cerr.precision(std::numeric_limits<T>::digits10);
                cerr << "\nVALIDATION FAILED!!!\n    reference" << "[" << i << ", " << j << "] = "
                     << golden << ",\n    calculated" << "[" << i << ", " << j << "] = "
                     << C_entry
                     << ",\n    absolute difference" << "[" << i << ", " << j << "] = " << absdiff << "\n"
                     << "Further validation was stopped\n\n";
                return false;
            }
        }
    }

    std::cout << " PASSED\n";
    return true;
}


// TODO: worry about alignment.
template <typename T>
std::tuple<T*, T*, T*> make_matrix(size_t size_a, size_t size_b, size_t size_c) {
  size_t mem_size_a = size_a * size_b * sizeof(T);
  size_t mem_size_b = size_b * size_c * sizeof(T);
  size_t mem_size_c = size_a * size_c * sizeof(T);

  T* A = (T*) new char[mem_size_a];
  T* B = (T*) new char[mem_size_b];
  T* C = (T*) new char[mem_size_c];

  for(size_t i = 0; i < size_a; ++i)
  {
    T* row_A = A + i * size_b;
    T* row_C = C + i * size_c;

    // Fill the rows with random values from range [0, 1]
    fill_rand_uniform_01(row_A, size_b);

    // we initialize C matrix with all zeros.
    std::fill(row_C, row_C + size_c, T(0));
  }

  for(size_t i = 0; i < size_b; ++i) {
    T* row_B = B + i * size_c;

    // Fill the rows with random values from range [0, 1]
    fill_rand_uniform_01(row_B, size_c);
  }

  return std::make_tuple(A, B, C);
}

struct BenchmarkResult {
  double time;
  double gflops;
  double energy;
  double power;
};

template <typename T, 
         typename FunctionSetup,
         typename FunctionLoadMatrix,
         typename FunctionRun,
         typename FunctionLoadResult,
         typename FunctionCleanup>
BenchmarkResult test(size_t size_a, size_t size_b, size_t size_c, 
    FunctionSetup setup, FunctionLoadMatrix loadMatrix, 
    FunctionRun run, FunctionLoadResult loadResult, FunctionCleanup cleanup) {

  double flops = double(size_a)* size_c * (
      size_b + // multiplications
      size_b   // additions
  );

  setup();
 
  vector<double> times, gflops;

  size_t num_iter = cmdparser->iterations.getValue();

  // for accurate energy gauge. 
  // we set energy = total_energy - baseline_energy.
  //    total_energy is the energy for making, loading matrix, run, etc.
  //    baseline_energy is the energy cost for everything except run.

  BenchmarkResult result;
  double baseline_energy = 0.0;

  for(int baseline = 1; baseline >= 0; baseline--) { 
    std::pair<double, double> first_time_energy = Mocha::getCurrentEnergy();

    if(baseline) {
      cout << "[benchmark] running baseline" << endl;
    }else{
      cout << "[benchmark] running benchmark" << endl;
    }

    double total_start, total_end, total_time;
    total_start = time_stamp();

    // Here we start measuring host time for kernel execution
    double start, end, time;
    auto ABC = make_matrix<T>(size_a, size_b, size_c);
    T* matrix_A = get<0>(ABC);
    T* matrix_B = get<1>(ABC);
    T* matrix_C = get<2>(ABC);

    // load matrix onto device memory.
    start = time_stamp();
    loadMatrix(matrix_A, matrix_B, matrix_C);
    end = time_stamp();
    time = (end - start) / num_iter;
    cout << "[Host] load matrix: " << time << " sec.\n";

    // start computation.
    if(!baseline) {
      start = time_stamp();
      run(num_iter);
      end = time_stamp();
      time = (end - start) / num_iter;
      cout << "[Host] GEMM: " << time << " sec.\n";
      cout << "[Host] GEMM perf: " << flops/time/1e9 << " GFLOPS\n";


      if(cmdparser->architecture.getValue() == "gpu") {  // use device time profile.
        double device_time = Mocha::GEMM_GPU<T>::getDeviceTime() / 1e9;
        cout << "[Device] GEMM: " << device_time << " sec.\n";
        cout << "[Device] GEMM perf: " << flops/device_time/1e9 << " GFLOPS\n";
        times.push_back(time);
        gflops.push_back(flops / time / 1e9);
        // times.push_back(device_time);
        // gflops.push_back(flops / device_time / 1e9);
      }else{
        times.push_back(time);
        gflops.push_back(flops / time / 1e9);
      }
    }

    cout.flush();

    loadResult();
    if(baseline && cmdparser->validation.getValue())
    {
       if(
            !checkValidity(
                matrix_A,
                matrix_B,
                matrix_C,
                size_a,
                size_b,
                size_c
            )
        )
        {
            throw Error("Validation procedure reported failures");
        }

        cout.flush();
    }

    delete[] matrix_A;
    delete[] matrix_B;
    delete[] matrix_C;

    total_end = time_stamp();
    total_time = total_end - total_start;

    // the energy profiler process runs at 1 sec interval.
    // we need to wait for a moment of quiescence.
    cout << "[sleep] wait for energy profile" << endl;
    sleep(2); 

    // benchmark energy.
    std::pair<double, double> last_time_energy = Mocha::getCurrentEnergy();

    double energy = (last_time_energy.second - first_time_energy.second) / (double)(num_iter);
    if(baseline) {
      baseline_energy = energy;
      cout << "[benchmark] baseline energy = " << baseline_energy << endl;
    }else{
      result.energy = energy - baseline_energy;
      cout << "[benchmark] total energy = " << result.energy << endl;
      result.power = result.energy * (double)num_iter / total_time;

      cout << "[benchmark] total power = " << result.power << endl;
    }
  }
  
  cleanup();
  result.time = 0.;
  result.gflops = 0.;

  for(auto time : times) {
    result.time += time;
  }
  result.time /= times.size();

  for(auto gf: gflops) {
    result.gflops += gf;
  }
  result.gflops /= gflops.size();
  
  return result;
}

void doNothing() {

}

template <typename T>
BenchmarkResult testGEMMGPU(size_t size_a, size_t size_b, size_t size_c) {
  return test<T>(size_a, size_b, size_c, 
            Mocha::GEMM_GPU<T>::setup,
            Mocha::GEMM_GPU<T>::loadMatrix,
            Mocha::GEMM_GPU<T>::run,
            Mocha::GEMM_GPU<T>::loadResult,
            Mocha::GEMM_GPU<T>::cleanup);
}


template <typename T>
BenchmarkResult testGEMMCPU(size_t size_a, size_t size_b, size_t size_c) {
  return test<T>(size_a, size_b, size_c, 
            Mocha::GEMM_CPU<T>::setup,
            Mocha::GEMM_CPU<T>::loadMatrix,
            Mocha::GEMM_CPU<T>::run,
            doNothing,
            Mocha::GEMM_CPU<T>::cleanup);
}


template <typename T>
BenchmarkResult testGEMMViennaCL(size_t size_a, size_t size_b, size_t size_c) {
  return test<T>(size_a, size_b, size_c, 
            Mocha::GEMM_VIENNACL<T>::setup,
            Mocha::GEMM_VIENNACL<T>::loadMatrix,
            Mocha::GEMM_VIENNACL<T>::run,
            Mocha::GEMM_VIENNACL<T>::loadResult,
            Mocha::GEMM_VIENNACL<T>::cleanup);
}


int main(int argc, const char* argv[]) {
  try
  {
    // Define and parse command-line arguments.
    cmdparser = std::make_shared<CmdParserMochaGEMM>(argc, argv);
    cmdparser->parse();

    // Immediatly exit if user wanted to see the usage information only.
    if(cmdparser->help.isSet())
    {
      return 0;
    }

    string architecture = cmdparser->architecture.getValue();
    
    cout << "[architecture] " << architecture << endl;

    if(cmdparser->baseline.getValue()) {
      cout << "[baseline] running" << endl;
    }
    
    string arithmetic = cmdparser->arithmetic.getValue();

    // matrix size parameters.
    size_t size_a = cmdparser->sa.getValue(),
           size_b = cmdparser->sb.getValue(),
           size_c = cmdparser->sc.getValue();
    
    cout << "[matrix size] " << size_a << " " 
      << size_b << " " << size_c << endl;

    // benchmark time and gflops. 
    BenchmarkResult result;
    if(architecture == "gpu") {
      if(arithmetic == "float") {
        result = testGEMMGPU<float>(size_a, size_b, size_c);
      }else if(arithmetic == "double") {
        result = testGEMMGPU<double>(size_a, size_b, size_c);
      }else if(arithmetic == "half") {
        result = testGEMMGPU<short>(size_a, size_b, size_c); // TODO: support half on host.
      }
    }else if(architecture == "cpu") {
      if(arithmetic == "float") {
        result = testGEMMCPU<float>(size_a, size_b, size_c);
      }else if(arithmetic == "double") {
        result = testGEMMCPU<double>(size_a, size_b, size_c);
      }
    }else if(architecture == "viennacl") {
      if(arithmetic == "float") {
        result = testGEMMViennaCL<float>(size_a, size_b, size_c);
      }else if(arithmetic == "double") {
        result = testGEMMViennaCL<double>(size_a, size_b, size_c);
      }
    }


    // output benchmark result.
    cout << "----------------Benchmark Results------------------" << endl;
    cout << "average energy = " << result.energy << endl;
    cout << "average time = " << result.time << endl;
    cout << "average gflops = " << result.gflops << endl;

    // write output to json.
    std::string output_filename = cmdparser->output.getValue();
    std::fstream output(output_filename, std::fstream::out);
    output << "{" << endl;
    output << "\t\"params\": {" << endl;
    output << "\t\t\"arithmetic\": \"" << cmdparser->arithmetic.getValue() << "\"," << endl;
    output << "\t\t\"sa\": " << cmdparser->sa.getValue() << "," << endl;
    output << "\t\t\"sb\": " << cmdparser->sb.getValue() << "," << endl;
    output << "\t\t\"sc\": " << cmdparser->sc.getValue() << "," << endl;
    if(architecture == "gpu") {
      output << "\t\t\"cl_program\": \"" << cmdparser->cl_program.getValue() << "\"," << endl;
    }
    output << "\t\t\"arch\": \"" << cmdparser->architecture.getValue() << "\"" << endl;
    output << "\t}," << endl;
    output << "\t\"energy\": " << result.energy << "," << endl;
    output << "\t\"power\": " << result.power << "," << endl;
    output << "\t\"time\": " << result.time << "," << endl;
    output << "\t\"gflops\": " << result.gflops << endl;
    output << "}" << endl;
    output.close();

    // tell PIPE that I'm done.
    cout << "[done]" << endl;

  }
  catch(const CmdParser::Error& error)
  {
      cerr
          << "[ ERROR ] In command line: " << error.what() << "\n"
          << "Run " << argv[0] << " -h for usage info.\n";
      return 1;
  }
  catch(const Error& error)
  {
      cerr << "[ ERROR ] Sample application specific error: " << error.what() << "\n";
      return 1;
  }
  catch(const exception& error)
  {
      cerr << "[ ERROR ] " << error.what() << "\n";
      return 1;
  }
  catch(...)
  {
      cerr << "[ ERROR ] Unknown/internal error happened.\n";
      return 1;
  }

  return 0;
}

