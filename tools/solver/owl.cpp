/*
 * Owl: SMT Solver Tool
 * Supports CNF and SMT-LIB2 format inputs
 */

#include "Solvers/SMT/LIBSMT/CNF.h"
#include "Solvers/SMT/LIBSMT/SATSolver.h"
#include "Solvers/SMT/LIBSMT/SMTFactory.h"
#include "Solvers/SMT/LIBSMT/SMTModel.h"
#include "Solvers/SMT/LIBSMT/SMTSolver.h"

#include <iostream>
// #include <memory>
#include <string>
// #include <cstdlib>

#ifdef _MSC_VER
#include <ctime>
double _get_cpu_time() { return (double)clock() / CLOCKS_PER_SEC; }
#else
#include <sys/time.h>
#endif

using namespace std;

static string input_file;
static string mode;
static bool verbose = false;
static bool show_stats = false;

void print_usage(const char *program_name) {
  cout << "Owl: SMT Solver Tool\n"
       << "Usage: " << program_name << " [OPTIONS]\n\n"
       << "Options:\n"
       << "  --cnf <file>     Solve CNF file\n"
       << "  --smt <file>     Solve SMT-LIB2 file\n"
       << "  --verbose        Enable verbose output\n"
       << "  --stats          Show solver statistics\n"
       << "  --help           Show this help message\n";
}

bool parse_arguments(int argc, char **argv) {
  if (argc < 2) {
    print_usage(argv[0]);
    return false;
  }

  for (int i = 1; i < argc; i++) {
    string arg = argv[i];

    if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return false;
    } else if (arg == "--cnf") {
      if (i + 1 >= argc) {
        cerr << "Error: --cnf requires a filename\n";
        return false;
      }
      mode = "cnf";
      input_file = argv[++i];
    } else if (arg == "--smt") {
      if (i + 1 >= argc) {
        cerr << "Error: --smt requires a filename\n";
        return false;
      }
      mode = "smt";
      input_file = argv[++i];
    } else if (arg == "--verbose") {
      verbose = true;
    } else if (arg == "--stats") {
      show_stats = true;
    } else {
      cerr << "Error: Unknown option '" << arg << "'\n";
      print_usage(argv[0]);
      return false;
    }
  }

  if (mode.empty()) {
    cerr << "Error: Must specify either --cnf or --smt mode\n";
    return false;
  }

  if (input_file.empty()) {
    cerr << "Error: Must specify input file\n";
    return false;
  }

  return true;
}

int solve_cnf(const string &filename) {
  if (verbose)
    cout << "Solving CNF file: " << filename << "\n";

  try {
    cnf *m_cnf = new cnf(const_cast<char *>(filename.c_str()));

    if (verbose) {
      cout << "CNF loaded: " << m_cnf->m_vc << " variables, " << m_cnf->m_cc
           << " clauses" << "\n";
    }

    sat_solver solver(*m_cnf);
    bool result = solver.run();

    if (show_stats)
      solver.print_stats();

    delete m_cnf;

    if (result) {
      cout << "s SATISFIABLE" << "\n";
      if (verbose)
        solver.print_solution(stdout);
      return 10;
    } else {
      cout << "s UNSATISFIABLE" << "\n";
      return 20;
    }
  } catch (const exception &e) {
    cerr << "Error solving CNF: " << e.what() << "\n";
    return 1;
  }
}

int solve_smt(const string &filename) {
  if (verbose)
    cout << "Solving SMT-LIB2 file: " << filename << "\n";

  try {
    SMTFactory factory;
    SMTSolver solver = factory.createSMTSolver();

    if (verbose)
      cout << "Parsing SMT-LIB2 file..." << "\n";

    SMTExpr expr = factory.parseSMTLib2File(filename);
    solver.add(expr);

    if (verbose)
      cout << "Checking satisfiability..." << "\n";

    SMTSolver::SMTResultType result = solver.check();

    switch (result) {
    case SMTSolver::SMTRT_Sat:
      cout << "s SATISFIABLE" << "\n";
      if (verbose) {
        SMTModel model = solver.getSMTModel();
        cout << "Model: " << model << "\n";
      }
      return 10;

    case SMTSolver::SMTRT_Unsat:
      cout << "s UNSATISFIABLE" << "\n";
      return 20;

    case SMTSolver::SMTRT_Unknown:
      cout << "s UNKNOWN" << "\n";
      return 30;

    case SMTSolver::SMTRT_Uncheck:
      cout << "s UNCHECKED" << "\n";
      return 40;

    default:
      cerr << "Error: Unexpected solver result" << "\n";
      return 1;
    }
  } catch (const exception &e) {
    cerr << "Error solving SMT: " << e.what() << "\n";
    return 1;
  }
}

int main(int argc, char **argv) {
  if (!parse_arguments(argc, argv))
    return 1;

  if (mode == "cnf") {
    return solve_cnf(input_file);
  } else if (mode == "smt") {
    return solve_smt(input_file);
  } else {
    cerr << "Error: Invalid mode '" << mode << "'" << "\n";
    return 1;
  }
}
