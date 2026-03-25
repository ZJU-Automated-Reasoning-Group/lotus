#pragma once

#include <string>
#include <vector>

#include <llvm/ADT/StringRef.h>

class FileUtil {
public:
  static bool createDirectory(llvm::StringRef Directory);
  static bool writeToFile(llvm::StringRef Path, llvm::StringRef ContentToWrite,
                          bool IsCreateDir = false);
  static bool getSubDirectories(llvm::StringRef Path,
                                std::vector<std::string> &SubDirectories);
  static std::string readStringFromFile(llvm::StringRef Path, bool &IsSuccess);
  static bool readCharVectorFromFile(llvm::StringRef Path,
                                     std::vector<char> &CharVector);
  static bool isFileExist(llvm::StringRef Path);
  static std::string getFileNameFromPath(llvm::StringRef path,
                                         llvm::StringRef separator = "/\\");
};