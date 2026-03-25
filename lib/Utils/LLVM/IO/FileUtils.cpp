#include "Utils/LLVM/FileUtils.h"

#include <fstream>

#include <llvm/Support/Debug.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

using namespace llvm;

// Creates a directory and all parent directories if they don't exist.
bool FileUtil::createDirectory(llvm::StringRef Directory) {
  if (llvm::sys::fs::create_directories(Directory)) {
    errs() << "\nFailed to create the directory: " << Directory << "\n";
    return false;
  }
  return true;
}

// Writes content to a file, optionally creating parent directories.
bool FileUtil::writeToFile(llvm::StringRef Path, llvm::StringRef ContentToWrite,
                           bool IsCreateDir) {
  if (llvm::sys::path::has_parent_path(Path)) {
    if (IsCreateDir) {
      std::string Directory = llvm::sys::path::parent_path(Path).str();
      if (!createDirectory(Directory)) {
        errs() << "\nFailed to write to the file: " << Path
               << " (fail to create parent directory)\n";
        return false;
      }
    }
  }
  int FD;
  if (llvm::sys::fs::openFileForWrite(Path, FD)) {
    errs() << "\nFailed to write to the file: " << Path
           << "(fail to create file)\n";
    return false;
  } else {
    llvm::raw_fd_ostream os(FD, true);
    os << ContentToWrite;
    os.flush();
    os.close();
  }
  return true;
}

// Gets all subdirectories of the given path.
bool FileUtil::getSubDirectories(llvm::StringRef Path,
                                 std::vector<std::string> &SubDirectories) {
  bool IsDir;
  if (llvm::sys::fs::is_directory(Path, IsDir)) {
    errs() << "\nFailed to judge the path " << Path << " is a directory\n";
    return false;
  } else {
    if (!IsDir) {
      errs() << "\n The path " << Path << " is not a directory\n";
      return false;
    }
  }

  std::error_code EC;
  llvm::sys::fs::directory_iterator DirIt(Path, EC);
  llvm::sys::fs::directory_iterator EndIt;
  while (!EC) {
    bool IsDirResult;
    llvm::sys::fs::is_directory(DirIt.operator->()->path(), IsDirResult);
    if (IsDirResult) {
      SubDirectories.push_back(std::string(DirIt.operator->()->path()));
    }
    DirIt.increment(EC);
    if (DirIt == EndIt) {
      break;
    }
  }
  return true;
}

// Reads the entire contents of a file as a string.
std::string FileUtil::readStringFromFile(llvm::StringRef Path,
                                         bool &IsSuccess) {
  int FD;
  if (llvm::sys::fs::openFileForRead(Path, FD)) {
    errs() << "\nFailed to read the file: " << Path
           << "(opening for reading)\n";
    IsSuccess = false;
    return "";
  }

  std::uint64_t FileSize;
  if (llvm::sys::fs::file_size(Path, FileSize)) {
    errs() << "\nFailed to read the file: " << Path << " (reading file size)\n";
    IsSuccess = false;
    return "";
  }

  if (!FileSize) {
    IsSuccess = true;
    return "";
  }

  std::error_code EC;
  llvm::sys::fs::mapped_file_region MappedFile(
      FD, llvm::sys::fs::mapped_file_region::mapmode::readonly, FileSize, 0,
      EC);
  if (EC) {
    errs() << "\nFailed to read the file: " << Path
           << " (reading file content)\n";
    IsSuccess = false;
    return "";
  }
  const char *Data = MappedFile.const_data();
  IsSuccess = true;
  return std::string(Data);
}

// Reads the entire contents of a file as a vector of characters.
bool FileUtil::readCharVectorFromFile(llvm::StringRef Path,
                                      std::vector<char> &CharVector) {

  if (!llvm::sys::fs::exists(Path)) {
    return false;
  }

  std::uint64_t FileSize;
  if (llvm::sys::fs::file_size(Path, FileSize)) {
    return false;
  }

  std::ifstream File(Path.str(), std::ios::binary);
  CharVector.reserve(FileSize);
  CharVector.assign(std::istreambuf_iterator<char>(File),
                    std::istreambuf_iterator<char>());
  File.close();
  return true;
}

// Returns true if the file or directory exists.
bool FileUtil::isFileExist(llvm::StringRef Path) {
  return llvm::sys::fs::exists(Path);
}

// Extracts the filename from a path using the given separator.
std::string FileUtil::getFileNameFromPath(llvm::StringRef path,
                                          llvm::StringRef separator) {
  size_t found = path.find_last_of(separator);
  if (found != std::string::npos) {
    return path.substr(found + 1).str();
  }
  return path.str();
}