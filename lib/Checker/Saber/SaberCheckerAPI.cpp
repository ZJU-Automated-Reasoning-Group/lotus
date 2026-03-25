//===- SaberCheckerAPI.cpp -- API for checkers in Saber--------------------//
//
// Migrated from SVF's SABER engine to Lotus.
// API list kept faithful to SVF's ei_pairs for alloc/free/fopen/fclose.
//
//===----------------------------------------------------------------------===//

#include "Checker/Saber/SaberCheckerAPI.h"

#include "Alias/TypeQualifier/Config.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <set>

namespace lotus {
namespace analysis {

SaberCheckerAPI *SaberCheckerAPI::ckAPI = nullptr;

namespace {

struct ei_pair {
  const char *n;
  SaberCheckerAPI::CHECKER_TYPE t;
};

static bool isModeledExternalSummaryName(llvm::StringRef name) {
  if (FunctionModelRegistry::lookup(name).kind != FunctionModelKind::Unknown)
    return true;

  static const std::array<const char *, 39> kAdditionalSummaryNames = {{
      "realloc",      "strlen",      "scanf",      "fprintf",
      "sprintf",      "snprintf",    "vprintf",    "vfprintf",
      "vsprintf",     "vsnprintf",   "recv",       "recvfrom",
      "itoa",         "strtok",      "strchr",     "strstr",
      "strpbrk",      "fgets",       "fread",      "fwrite",
      "time",         "getenv",      "strtod",     "strtof",
      "strtold",      "strtol",      "strtoll",    "strtoul",
      "strtoull",     "atoi",        "atol",       "atoll",
      "strdup",       "strndup",     "reallocarray",
      "posix_memalign", "asprintf",  "vasprintf",  "strtok_r",
  }};

  for (const char *candidate : kAdditionalSummaryNames) {
    if (name == candidate)
      return true;
  }

  return false;
}

} // namespace

// Each (name, type) pair is inserted into the map.
// All entries of the same type must occur together (for error detection).
static const ei_pair ei_pairs[] = {
    {"alloc", SaberCheckerAPI::CK_ALLOC},
    {"alloc_check", SaberCheckerAPI::CK_ALLOC},
    {"alloc_clear", SaberCheckerAPI::CK_ALLOC},
    {"calloc", SaberCheckerAPI::CK_ALLOC},
    {"jpeg_alloc_huff_table", SaberCheckerAPI::CK_ALLOC},
    {"jpeg_alloc_quant_table", SaberCheckerAPI::CK_ALLOC},
    {"lalloc", SaberCheckerAPI::CK_ALLOC},
    {"lalloc_clear", SaberCheckerAPI::CK_ALLOC},
    {"malloc", SaberCheckerAPI::CK_ALLOC},
    {"nhalloc", SaberCheckerAPI::CK_ALLOC},
    {"oballoc", SaberCheckerAPI::CK_ALLOC},
    {"permalloc", SaberCheckerAPI::CK_ALLOC},
    {"png_create_info_struct", SaberCheckerAPI::CK_ALLOC},
    {"png_create_write_struct", SaberCheckerAPI::CK_ALLOC},
    {"safe_calloc", SaberCheckerAPI::CK_ALLOC},
    {"safe_malloc", SaberCheckerAPI::CK_ALLOC},
    {"safecalloc", SaberCheckerAPI::CK_ALLOC},
    {"safemalloc", SaberCheckerAPI::CK_ALLOC},
    {"safexcalloc", SaberCheckerAPI::CK_ALLOC},
    {"safexmalloc", SaberCheckerAPI::CK_ALLOC},
    {"savealloc", SaberCheckerAPI::CK_ALLOC},
    {"xalloc", SaberCheckerAPI::CK_ALLOC},
    {"xcalloc", SaberCheckerAPI::CK_ALLOC},
    {"xmalloc", SaberCheckerAPI::CK_ALLOC},
    {"SSL_CTX_new", SaberCheckerAPI::CK_ALLOC},
    {"SSL_new", SaberCheckerAPI::CK_ALLOC},
    {"VOS_MemAlloc", SaberCheckerAPI::CK_ALLOC},

    {"VOS_MemFree", SaberCheckerAPI::CK_FREE},
    {"cfree", SaberCheckerAPI::CK_FREE},
    {"free", SaberCheckerAPI::CK_FREE},
    {"free_all_mem", SaberCheckerAPI::CK_FREE},
    {"freeaddrinfo", SaberCheckerAPI::CK_FREE},
    {"gcry_mpi_release", SaberCheckerAPI::CK_FREE},
    {"gcry_sexp_release", SaberCheckerAPI::CK_FREE},
    {"globfree", SaberCheckerAPI::CK_FREE},
    {"nhfree", SaberCheckerAPI::CK_FREE},
    {"obstack_free", SaberCheckerAPI::CK_FREE},
    {"safe_cfree", SaberCheckerAPI::CK_FREE},
    {"safe_free", SaberCheckerAPI::CK_FREE},
    {"safefree", SaberCheckerAPI::CK_FREE},
    {"safexfree", SaberCheckerAPI::CK_FREE},
    {"sm_free", SaberCheckerAPI::CK_FREE},
    {"vim_free", SaberCheckerAPI::CK_FREE},
    {"xfree", SaberCheckerAPI::CK_FREE},
    {"SSL_CTX_free", SaberCheckerAPI::CK_FREE},
    {"SSL_free", SaberCheckerAPI::CK_FREE},
    {"XFree", SaberCheckerAPI::CK_FREE},

    {"fopen", SaberCheckerAPI::CK_FOPEN},
    {"\01_fopen", SaberCheckerAPI::CK_FOPEN},
    {"\01fopen64", SaberCheckerAPI::CK_FOPEN},
    {"\01readdir64", SaberCheckerAPI::CK_FOPEN},
    {"\01tmpfile64", SaberCheckerAPI::CK_FOPEN},
    {"fopen64", SaberCheckerAPI::CK_FOPEN},
    {"XOpenDisplay", SaberCheckerAPI::CK_FOPEN},
    {"XtOpenDisplay", SaberCheckerAPI::CK_FOPEN},
    {"fopencookie", SaberCheckerAPI::CK_FOPEN},
    {"popen", SaberCheckerAPI::CK_FOPEN},
    {"readdir", SaberCheckerAPI::CK_FOPEN},
    {"readdir64", SaberCheckerAPI::CK_FOPEN},
    {"gzdopen", SaberCheckerAPI::CK_FOPEN},
    {"iconv_open", SaberCheckerAPI::CK_FOPEN},
    {"tmpfile", SaberCheckerAPI::CK_FOPEN},
    {"tmpfile64", SaberCheckerAPI::CK_FOPEN},
    {"BIO_new_socket", SaberCheckerAPI::CK_FOPEN},
    {"gcry_md_open", SaberCheckerAPI::CK_FOPEN},
    {"gcry_cipher_open", SaberCheckerAPI::CK_FOPEN},

    {"fclose", SaberCheckerAPI::CK_FCLOSE},
    {"XCloseDisplay", SaberCheckerAPI::CK_FCLOSE},
    {"XtCloseDisplay", SaberCheckerAPI::CK_FCLOSE},
    {"__res_nclose", SaberCheckerAPI::CK_FCLOSE},
    {"pclose", SaberCheckerAPI::CK_FCLOSE},
    {"closedir", SaberCheckerAPI::CK_FCLOSE},
    {"dlclose", SaberCheckerAPI::CK_FCLOSE},
    {"gzclose", SaberCheckerAPI::CK_FCLOSE},
    {"iconv_close", SaberCheckerAPI::CK_FCLOSE},
    {"gcry_md_close", SaberCheckerAPI::CK_FCLOSE},
    {"gcry_cipher_close", SaberCheckerAPI::CK_FCLOSE},

    // Sentinel: must be last (SVF uses {0, CK_DUMMY}); loop stops when p->n is
    // null.
    {nullptr, SaberCheckerAPI::CK_DUMMY}};

void SaberCheckerAPI::init() {
  std::set<CHECKER_TYPE> t_seen;
  CHECKER_TYPE prev_t = CK_DUMMY;
  t_seen.insert(CK_DUMMY);
  for (const ei_pair *p = ei_pairs; p->n; ++p) {
    if (p->t != prev_t) {
      if (t_seen.count(p->t)) {
        fputs(p->n, stderr);
        putc('\n', stderr);
        assert(!"ei_pairs not grouped by type");
      }
      t_seen.insert(p->t);
      prev_t = p->t;
    }
    if (tdAPIMap.count(p->n)) {
      fputs(p->n, stderr);
      putc('\n', stderr);
      assert(!"duplicate name in ei_pairs");
    }
    tdAPIMap[p->n] = p->t;
  }
}

bool SaberCheckerAPI::isExtCall(const llvm::Function *fun) const {
  if (!fun)
    return false;

  // Match SVF's ExtAPI behavior used by SABER:
  //   - declarations and intrinsics are external
  //   - modeled summary functions are external even if a body is available
  //   - available_externally bodies should still be analyzed like ordinary code
  if (fun->hasAvailableExternallyLinkage())
    return false;

  if (fun->isIntrinsic() || fun->isDeclaration())
    return true;

  const std::string funName = fun->getName().str();
  if (tdAPIMap.find(funName) != tdAPIMap.end())
    return true;

  return isModeledExternalSummaryName(fun->getName());
}

} // namespace analysis
} // namespace lotus
