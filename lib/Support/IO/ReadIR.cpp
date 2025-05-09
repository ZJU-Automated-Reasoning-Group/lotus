#include "Support/IO/ReadIR.h"

#include <llvm/AsmParser/Parser.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/SourceMgr.h>

using namespace llvm;

namespace util
{
namespace io
{

std::unique_ptr<Module> readModuleFromFile(const char* fileName)
{
	static LLVMContext context;
	SMDiagnostic error;
	auto module = parseIRFile(fileName, error, context);

	auto errMsg = std::string();
	llvm::raw_string_ostream os(errMsg);
	error.print("", os);

	if (!module)
	{
		// A failure here means that the test itself is buggy.
		report_fatal_error(os.str().c_str());
	}

	return module;
}

std::unique_ptr<Module> parseAssembly(const char *assembly)
{
	static LLVMContext context;
	SMDiagnostic error;
	auto module = parseAssemblyString(assembly, error, context);

	auto errMsg = std::string();
	llvm::raw_string_ostream os(errMsg);
	error.print("", os);

	if (!module)
	{
		// A failure here means that the test itself is buggy.
		report_fatal_error(os.str().c_str());
	}

	return module;
}

}
}
