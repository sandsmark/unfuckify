#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <vector>


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
    void handleAutoToken(CXToken *autoToken)
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

        replacements.push_back(std::move(replacement));
    }

    bool process(const std::string &compilePath, const std::string &sourceFile)
    {
        std::cout << "Processing " << sourceFile << std::endl;

        CXCompilationDatabase_Error compilationDatabaseError;
        CXCompilationDatabase compilationDatabase = clang_CompilationDatabase_fromDirectory(
                    compilePath.c_str(),
                    &compilationDatabaseError
                );

        if (compilationDatabaseError != CXCompilationDatabase_NoError) {
            std::cerr << "Failed to load " << compilePath << "(err " << compilationDatabaseError << ")" << std::endl;
            return false;
        }

        CXCompileCommands compileCommands = clang_CompilationDatabase_getCompileCommands(
                                                compilationDatabase,
                                                sourceFile.c_str()
                                            );

        if (!compileCommands) {
            std::cerr << "Failed to find compile command for " << sourceFile << " in " << compilePath << std::endl;
            return false;
        }

        size_t numCompileCommands = clang_CompileCommands_getSize(compileCommands);
        if (numCompileCommands == 0) {
            std::cerr << "No compile commands found for " << sourceFile << std::endl;
            return false;
        }

        CXCompileCommand compileCommand = clang_CompileCommands_getCommand(compileCommands, 0);
        size_t numArguments = clang_CompileCommand_getNumArgs(compileCommand);
        char **arguments = new char *[numArguments];

        std::cout << "Compile commands:";
        for (size_t i = 0; i < numArguments; i++) {
            CXString argument = clang_CompileCommand_getArg(compileCommand, i);
            std::string strArgument = clang_getCString(argument);
            arguments[i] = new char[ strArgument.size() + 1 ];
            std::fill(arguments[i], arguments[i] + strArgument.size() + 1, 0);

            std::copy(strArgument.begin(), strArgument.end(), arguments[i]);

            std::cout << " " << arguments[i] << std::flush;

            clang_disposeString(argument);
        }
        std::cout << std::endl;
        clang_CompileCommands_dispose(compileCommands);
        clang_CompilationDatabase_dispose(compilationDatabase);

        CXIndex index = clang_createIndex(
                            0,
                            1 // Print warnings and errors
                        );

        // Need to use the full argv thing for clang to pick up default paths
        CXErrorCode parseError = clang_parseTranslationUnit2FullArgv(
            index,
            0,
            arguments,
            numArguments,
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

        for (unsigned i = 0; i < numTokens; i++) {
            if (clang_getTokenKind(tokens[i]) != CXToken_Keyword) {
                continue;
            }

            if (getString(clang_getTokenSpelling(translationUnit, tokens[i])) == "auto") {
                handleAutoToken(&tokens[i]);
                break;
            }
        }

        for (size_t i = 0; i < numArguments; i++) {
            delete[] arguments[i];
        }

        delete[] arguments;
        clang_disposeIndex(index);

        return true;
    }

    bool fixFile(const std::string &filePath)
    {
        if (replacements.empty()) {
            std::cerr << "Nothing to fix" << std::endl;
            return false;
        }

        FILE *file = fopen(filePath.c_str(), "r");

        if (!file) {
            std::cerr << "Failed to open file " << filePath << std::endl;
            return false;
        }

        fseek(file, 0, SEEK_END);
        const long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        FILE *outFile = fopen((filePath + "-fixed").c_str(), "w");

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

        return true;
    }

    struct Replacement {
        unsigned start, end;
        std::string string;
    };
    std::vector<Replacement> replacements;

    CXTranslationUnit translationUnit;
    int childrenVisited = 0;
};

static void printUsage(const std::string &executable)
{
    std::cerr << "Please pass a compile_commands.json and a source file to fix" << std::endl;
    std::cerr << executable << " path/to/compile_commands.json path/to/heretical.cpp" << std::endl;
    std::cerr << "To create a compilation database run cmake with '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON' on the project you're going to fix" << std::endl;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::filesystem::path compileDbPath(argv[1]);
    if (!std::filesystem::exists(compileDbPath)) {
        std::cerr << argv[1] << " does not exist" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    if (compileDbPath.filename() != "compile_commands.json") {
        std::cerr << argv[1] << " is not a compile_commands.json file" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    compileDbPath.remove_filename();

    std::filesystem::path sourceFile(argv[2]);
    if (!std::filesystem::exists(sourceFile)) {
        std::cerr << "source file " << sourceFile << " does not exist" << std::endl;
        printUsage(argv[0]);
        return 1;
    }
    sourceFile = std::filesystem::absolute(sourceFile);

    Unfuckifier fixer;

    if (!fixer.process(compileDbPath.string(), sourceFile.string())) {
        std::cerr << "Failed to parse process " << sourceFile << std::endl;
        return 1;
    }

    if (!fixer.fixFile(sourceFile)) {
        std::cerr << "Failed to fix " << sourceFile << std::endl;
        return 1;
    }

    return 0;
}
