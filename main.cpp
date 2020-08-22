#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <vector>

#include <cstring>

extern "C" {
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>
}

std::ostream &operator<<(std::ostream &stream, const CXString &str)
{
    stream << clang_getCString(str);
    clang_disposeString(str);
    return stream;
}

std::string getString(const CXString &str)
{
    std::string ret(clang_getCString(str));
    clang_disposeString(str);
    return ret;
}

struct Unfuckifier {
    struct Replacement {
        unsigned start, end;
        std::string string;
    };

    static Replacement handleAutoToken(CXToken *autoToken, CXTranslationUnit translationUnit)
    {
        CXCursor cursor;
        clang_annotateTokens(translationUnit, autoToken, 1, &cursor);

        Replacement replacement;
        replacement.string = getString(clang_getTypeSpelling((clang_getCursorType(cursor))));
        CXSourceRange extent = clang_getTokenExtent(translationUnit, *autoToken);

        const CXSourceLocation start = clang_getRangeStart(extent);
        const CXSourceLocation end = clang_getRangeEnd(extent);
        clang_getSpellingLocation(start, nullptr, nullptr, nullptr, &replacement.start);
        clang_getSpellingLocation(end, nullptr, nullptr, nullptr, &replacement.end);

        return replacement;
    }

    bool parseCompilationDatabase(const std::string &compilePath)
    {
        CXCompilationDatabase_Error compilationDatabaseError;
        compilationDatabase = clang_CompilationDatabase_fromDirectory(
                    compilePath.c_str(),
                    &compilationDatabaseError
                );

        if (compilationDatabaseError != CXCompilationDatabase_NoError) {
            std::cerr << "Failed to load " << compilePath << "(err " << compilationDatabaseError << ")" << std::endl;
            return false;
        }
        return true;
    }

    std::vector<std::string> allAvailableFiles()
    {
        std::vector<std::string> files;

        CXCompileCommands compileCommands = clang_CompilationDatabase_getAllCompileCommands(compilationDatabase);
        const size_t numCompileCommands = clang_CompileCommands_getSize(compileCommands);
        if (numCompileCommands == 0) {
            std::cerr << "No compile commands found" << std::endl;
            return {};
        }
        for (size_t i=0; i<numCompileCommands; i++) {
            CXCompileCommand compileCommand = clang_CompileCommands_getCommand(compileCommands, i);
            files.push_back(getString(clang_CompileCommand_getFilename(compileCommand)));
        }
        clang_CompileCommands_dispose(compileCommands);
        return files;
    }

    ~Unfuckifier() {
        clang_CompilationDatabase_dispose(compilationDatabase);
    }

    bool process(const std::string &sourceFile)
    {
        std::cout << "Processing " << sourceFile << std::endl;

        CXCompileCommands compileCommands = clang_CompilationDatabase_getCompileCommands(
                                                compilationDatabase,
                                                sourceFile.c_str()
                                            );

        if (!compileCommands) {
            std::cerr << "Failed to find compile command for " << sourceFile << std::endl;
            return false;
        }


        CXCompileCommand compileCommand = clang_CompileCommands_getCommand(compileCommands, 0);
        std::cout << clang_CompileCommand_getFilename(compileCommand) << std::endl;

        std::vector<char*> arguments(clang_CompileCommand_getNumArgs(compileCommand));

        std::cout << "Compile commands:";
        for (size_t i = 0; i < arguments.size(); i++) {
            CXString argument = clang_CompileCommand_getArg(compileCommand, i);
            arguments[i] = strdup(clang_getCString(argument));

            std::cout << " " << arguments[i] << std::flush;

            clang_disposeString(argument);
        }
        std::cout << std::endl;
        clang_CompileCommands_dispose(compileCommands);

        CXIndex index = clang_createIndex(
                            0,
                            1 // Print warnings and errors
                        );

        CXTranslationUnit translationUnit{};

        // Need to use the full argv thing for clang to pick up default paths
        CXErrorCode parseError = clang_parseTranslationUnit2FullArgv(
            index,
            0,
            arguments.data(),
            arguments.size(),
            nullptr,
            0,
            CXTranslationUnit_None, // flags
            &translationUnit
        );

        if (parseError != CXError_Success) {
            std::cerr << "Failed to parse " << sourceFile << ": ";
            switch(parseError) {
            case CXError_Failure:
                std::cerr << "Generic unknown error (clang doesn't tell me more)" << std::endl;
                break;
            case CXError_Crashed:
                std::cerr << "libclang crashed (then how did it return this to me?)" << std::endl;
                break;
            case CXError_InvalidArguments:
                std::cerr << "Invalid arguments passed to parser (blame me)" << std::endl;
                break;
            case CXError_ASTReadError:
                std::cerr << "AST deserialization error (should never happen, I don't use it)" << std::endl;
                break;
            default:
                std::cerr << "Unknown error " << parseError << std::endl;
                break;
            }
            return false;
        }


        if (!translationUnit) {
            std::cerr << "Unable to parse translation unit. Quitting." << std::endl;
            return false;
        }

        // auto for testing
        auto cursor = clang_getTranslationUnitCursor(translationUnit);

        // We can't traverse the AST, because auto's are already resolved
        unsigned numTokens;
        CXToken *tokens = nullptr;
        clang_tokenize(translationUnit, clang_getCursorExtent(cursor), &tokens, &numTokens);

        CXToken *autoToken = nullptr;

        std::vector<Replacement> replacements;
        for (unsigned i = 0; i < numTokens; i++) {
            if (clang_getTokenKind(tokens[i]) != CXToken_Keyword) {
                continue;
            }

            if (getString(clang_getTokenSpelling(translationUnit, tokens[i])) == "auto") {
                replacements.push_back(handleAutoToken(&tokens[i], translationUnit));
            }
        }
        clang_disposeTokens(translationUnit, tokens, numTokens);

        for (size_t i = 0; i < arguments.size(); i++) {
            free(arguments[i]);
        }

        clang_disposeIndex(index);
        clang_disposeTranslationUnit(translationUnit);

        if (replacements.empty()) {
            return true;
        }

        return fixFile(sourceFile, replacements);
    }

    bool fixFile(const std::string &filePath, const std::vector<Replacement> &replacements)
    {
        if (replacements.empty()) {
            std::cerr << "Nothing to fix" << std::endl;
            return false;
        }

        const std::string outFilePath = filePath + "-fixed";

        FILE *file = fopen(filePath.c_str(), "r");

        if (!file) {
            std::cerr << "Failed to open file " << filePath << std::endl;
            return false;
        }

        fseek(file, 0, SEEK_END);
        const long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        FILE *outFile = fopen(outFilePath.c_str(), "w");

        if (!outFile) {
            std::cerr << "Failed to open out file " << filePath << std::endl;
            fclose(file);
            return false;
        }

        std::cout << "fixing " << replacements.size() << std::endl;

        std::vector<char> buffer;
        int lastPos = 0;

        for (const Replacement &replacement : replacements) {
            buffer.resize(replacement.start - lastPos);
            fread(buffer.data(), buffer.size(), 1, file);
            fwrite(buffer.data(), buffer.size(), 1, outFile);
            fwrite(replacement.string.c_str(), replacement.string.size(), 1, outFile);

            lastPos = replacement.end;
            fseek(file, replacement.end - replacement.start, SEEK_CUR);
        }

        if (lastPos < fileSize) {
            buffer.resize(fileSize - lastPos);
            fread(buffer.data(), buffer.size(), 1, file);
            fwrite(buffer.data(), buffer.size(), 1, outFile);
        }

        fclose(file);
        fclose(outFile);

        if (!replaceFile) {
            return true;
        }

        const std::filesystem::path backupFilePath = filePath + ".backup";
        if (std::filesystem::exists(backupFilePath)) {
            std::filesystem::remove(backupFilePath);
        }
        std::filesystem::rename(filePath, backupFilePath);
        std::filesystem::rename(outFilePath, filePath);

        return true;
    }
    bool replaceFile = false;

    CXCompilationDatabase compilationDatabase{};
    int childrenVisited = 0;

    bool verbose = false;
};

static void printUsage(const std::string &executable)
{
    std::cerr << "Please pass a compile_commands.json and a source file, or --all to fix all files in project" << std::endl;
    std::cerr << "To replace the existing files pass --replace" << std::endl;
    std::cerr << executable << " path/to/compile_commands.json [--replace] [--all] [path/to/heretical.cpp]" << std::endl;
    std::cerr << "To create a compilation database run cmake with '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON' on the project you're going to fix" << std::endl;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::filesystem::path compileDbPath;
    std::filesystem::path sourceFile;

    Unfuckifier fixer;
    bool all = false;
    for (int argNum = 1; argNum < argc; argNum++) {
        const std::string arg = argv[argNum];
        std::cout << arg << std::endl;

        if (arg == "--all") {
            all = true;
            continue;
        }

        if (arg == "--replace") {
            fixer.replaceFile = true;
            continue;
        }
        std::filesystem::path path(arg);
        if (!std::filesystem::exists(path)) {
            continue;
        }
        if (path.filename() == "compile_commands.json") {
            compileDbPath = path;
            continue;
        }
        sourceFile = path;
    }

    if (compileDbPath.empty() || !std::filesystem::exists(compileDbPath)) {
        std::cerr << "Please pass path to a compile_commands.json file" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    compileDbPath.remove_filename();
    std::cout << "WAnted all" << std::endl;

    fixer.parseCompilationDatabase(compileDbPath.string());

    if (all) {
        for (const std::string &file : fixer.allAvailableFiles()) {
            if (!fixer.process(file)) {
                std::cerr << "Failed to process " << sourceFile << std::endl;
                return 1;
            }
        }
    } else {
        if (sourceFile.empty() || !std::filesystem::exists(sourceFile)) {
            std::cerr << "Please pass a path to a source file" << std::endl;
            printUsage(argv[0]);
            return 1;
        }

        sourceFile = std::filesystem::absolute(sourceFile);

        if (!fixer.process(sourceFile.string())) {
            std::cerr << "Failed to process " << sourceFile << std::endl;
            return 1;
        }
    }

    return 0;
}
