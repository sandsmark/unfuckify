#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <vector>

#include <cstring>

extern "C" {
#include <unistd.h>
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

    bool fileContainsAuto(const std::string &filename)
    {
        FILE *file = fopen(filename.c_str(), "r");
        if (!file) {
            std::cerr << "Failed to open " << filename << " to check for auto" << std::endl;
            return false;
        }

        fseek(file, 0, SEEK_END);
        const long fileSize = ftell(file);
        fseek(file, 0, SEEK_SET);

        if (fileSize <= 0) {
            fclose(file);
            std::cerr << "Failed to get size of " << filename << " to check for auto" << std::endl;
            return false;
        }

        std::vector<char> buffer(fileSize);
        if (fread(buffer.data(), buffer.size(), 1, file) != 1) {
            std::cerr << "Short read from " << filename << " when checking for auto" << std::endl;
            fclose(file);
            return false;
        }
        fclose(file);

        if (!memmem(buffer.data(), buffer.size(), "auto", strlen("auto"))) {
            return false;
        }

        return true;
    }

    Replacement handleAutoToken(CXToken *autoToken, CXTranslationUnit translationUnit)
    {
        CXCursor cursor;
        clang_annotateTokens(translationUnit, autoToken, 1, &cursor);

        CXType type = clang_getCursorType(cursor);
        CXType pointerType = clang_getPointeeType(type);

        if (dumpNodes) {
            std::cout << " > type " << clang_getTypeSpelling(type) << std::endl;
            std::cout << " > kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
            std::cout << " > cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
            std::cout << " > pointer type " << clang_getTypeSpelling(pointerType) << std::endl;

            CXSourceRange extent = clang_getCursorExtent(cursor);
            const CXSourceLocation start = clang_getRangeStart(extent);
            const CXSourceLocation end = clang_getRangeEnd(extent);
            unsigned lineStart, lineEnd;
            unsigned colStart, colEnd;
            CXFile fileStart, fileEnd;
            clang_getSpellingLocation(start, &fileStart, &lineStart, &colStart, nullptr);
            clang_getSpellingLocation(end, &fileEnd, &lineEnd, &colEnd, nullptr);

            std::cerr << " > Starts at " << clang_getFileName(fileStart) << ":" << lineStart << ":" << colStart << std::endl;
            std::cerr << " > Ends at " << clang_getFileName(fileEnd) << ":" << lineEnd << ":" << colEnd << std::endl;
        }

        // In case of pointers, clang is a bit confused with 'auto *'  and
        // gives us 'foo *' instead of just 'foo', but the token extent covers
        // only 'auto' So resolve the pointer/reference type, if available, and
        // just use that.
        while (type.kind == CXType_Pointer || type.kind == CXType_RValueReference || type.kind == CXType_LValueReference) {
            CXType pointerType = clang_getPointeeType(type);
            if (pointerType.kind != CXType_Invalid) {
                type = pointerType;
            } else {
                std::cerr << "Got invalid pointer type" << std::endl;
            }
        }

        Replacement replacement;
        replacement.string = getString(clang_getTypeSpelling(type));

        CXSourceRange extent = clang_getTokenExtent(translationUnit, *autoToken);

        const CXSourceLocation start = clang_getRangeStart(extent);
        const CXSourceLocation end = clang_getRangeEnd(extent);
        clang_getSpellingLocation(start, nullptr, nullptr, nullptr, &replacement.start);
        clang_getSpellingLocation(end, nullptr, nullptr, nullptr, &replacement.end);

        // Same as with pointers, the token extent only covers `auto`, but
        // clang_getTypeSpelling() returns the whole shebang with const and &
        // and what have you.
        if (clang_isConstQualifiedType(type)) {
            std::string::size_type constStart = replacement.string.find("const ");
            if (constStart == std::string::npos) {
                std::cerr << "Failed to find position of const!" << std::endl;
                std::cout << " > type " << clang_getTypeSpelling(type) << std::endl;
                std::cout << " > kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
                std::cout << " > cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
                std::cout << " > pointer type " << clang_getTypeSpelling(pointerType) << std::endl;
                std::cout << " > pointer type is const " << clang_isConstQualifiedType(pointerType) << std::endl;

                CXSourceRange extent = clang_getCursorExtent(cursor);
                const CXSourceLocation start = clang_getRangeStart(extent);
                const CXSourceLocation end = clang_getRangeEnd(extent);
                unsigned lineStart, lineEnd;
                unsigned colStart, colEnd;
                CXFile fileStart, fileEnd;
                clang_getSpellingLocation(start, &fileStart, &lineStart, &colStart, nullptr);
                clang_getSpellingLocation(end, &fileEnd, &lineEnd, &colEnd, nullptr);

                std::cerr << " > Starts at " << clang_getFileName(fileStart) << ":" << lineStart << ":" << colStart << std::endl;
                std::cerr << " > Ends at " << clang_getFileName(fileEnd) << ":" << lineEnd << ":" << colEnd << std::endl;
            } else if (constStart != 0) {
                std::cout << "unexpected position of const modifier: " << constStart << std::endl;
                return {};
            } else {
                replacement.string = replacement.string.substr(strlen("const "));
            }
        }

        if (replacement.string.find("(lambda at ") != std::string::npos) {
            extent = clang_getCursorExtent(cursor);
            const CXSourceLocation start = clang_getRangeStart(extent);
            const CXSourceLocation end = clang_getRangeEnd(extent);
            unsigned lineStart, lineEnd;
            CXFile fileStart, fileEnd;
            clang_getSpellingLocation(start, &fileStart, &lineStart, nullptr, nullptr);
            clang_getSpellingLocation(end, &fileEnd, &lineEnd, nullptr, nullptr);

            std::cerr << std::endl;
            std::cerr << " !! Horribly broken crap, please rewrite manually by splitting out into functions" << std::endl;
            std::cerr << " -> Starts at " << clang_getFileName(fileStart) << ":" << lineStart << std::endl;
            std::cerr << " <- Ends at " << clang_getFileName(fileEnd) << ":" << lineEnd << std::endl;
            return {};
        }

        // Couldn't find a way to get libclang to mush together >>, so fix the
        // code style manually.
        // Not the pretties way, but fuck you if you judge me for it :))
        std::string::size_type braceSpace = replacement.string.find(" >");
        while (braceSpace != std::string::npos) {
            replacement.string.replace(braceSpace, strlen(" >"), ">");
            braceSpace = replacement.string.find(" >");
        }


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
            std::string filename = getString(clang_CompileCommand_getFilename(compileCommand));

            if (skipBuildDir) {
                std::string buildDir = getString(clang_CompileCommand_getDirectory(compileCommand));
                if (filename.find(buildDir) == 0) {
                    if (verbose) std::cout << "Skipping generated " << filename << std::endl;
                    else std::cout << "Skipping generated " << std::filesystem::path(filename).filename() << std::endl;
                    continue;
                }
            }
            if (!std::filesystem::exists(filename)) {
                if (verbose) std::cout << "Skipping nonexistent " << filename << std::endl;
                else std::cout << "Skipping nonexistent " << std::filesystem::path(filename).filename() << std::endl;
                continue;
            }
            files.push_back(filename);
        }
        clang_CompileCommands_dispose(compileCommands);
        return files;
    }

    ~Unfuckifier() {
        if (index) {
            clang_disposeIndex(index);
        }
        if (translationUnit) {
            clang_disposeTranslationUnit(translationUnit);
        }
        clang_CompilationDatabase_dispose(compilationDatabase);
    }

    bool process(const std::string &sourceFile)
    {
        const std::string filename = std::filesystem::path(sourceFile).filename();
        // Quickly scan the file to see if it contains the string "auto"
        if (skipHeaders && !fileContainsAuto(sourceFile)) {
            std::cout << filename << " does not contain 'auto'" << std::endl;
            return true;
        }

        std::cout << "Processing ";
        if (verbose) std::cout << sourceFile;
        else std::cout << filename;
        std::cout << "... " << std::flush;

        CXCompileCommands compileCommands = clang_CompilationDatabase_getCompileCommands(
                                                compilationDatabase,
                                                sourceFile.c_str()
                                            );

        if (!compileCommands) {
            std::cerr << "Failed to find compile command for " << sourceFile << std::endl;
            return false;
        }


        CXCompileCommand compileCommand = clang_CompileCommands_getCommand(compileCommands, 0);

        std::vector<char*> arguments(clang_CompileCommand_getNumArgs(compileCommand));

        if (verbose) std::cout << "Compile commands:";
        for (size_t i = 0; i < arguments.size(); i++) {
            std::string argument = getString(clang_CompileCommand_getArg(compileCommand, i));

            if (i == 0 && argument.find("ccache") != std::string::npos) {
                if (argument.find("clang++") != std::string::npos) {
                    arguments[i] = strdup("/usr/bin/clang++");
                } else {
                    arguments[i] = strdup("/usr/bin/clang");
                }
            } else {
                arguments[i] = strdup(argument.c_str());
            }

            if (verbose) std::cout << " " << arguments[i] << std::flush;
        }

        if (verbose) std::cout << std::endl;

        clang_CompileCommands_dispose(compileCommands);

        index = clang_createIndex(
                            0,
                            1 // Print warnings and errors
                        );


        // Need to use the full argv thing for clang to pick up default paths
        CXErrorCode parseError = clang_parseTranslationUnit2FullArgv(
            index,
            // source file, we can't pass this or it won't tell us that it failed (wtf)
            // If we don't pass it, however, it doesn't return an error code
            nullptr,
            arguments.data(),
            arguments.size(),
            nullptr, // unsaved_files
            0, // num_unsaved_files
            CXTranslationUnit_None, // flags
            &translationUnit
        );

        for (unsigned i=0; i<clang_getNumDiagnostics(translationUnit); i++) {
            CXDiagnostic diagnostic = clang_getDiagnostic(translationUnit, i);
            const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
            clang_disposeDiagnostic(diagnostic);

            if (severity >= CXDiagnostic_Error) {
                return false;
            }
        }

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
                std::cerr << "AST deserialization error (compile error?)" << std::endl;
                break;
            default:
                std::cerr << "Unknown error " << parseError << std::endl;
                break;
            }
            return false;
        }


        if (!translationUnit) {
            std::cerr << "No parse error, but didn't get a translation unit for " << sourceFile << std::endl;
            return false;
        }

        // auto for testing
        auto cursor = clang_getTranslationUnitCursor(translationUnit);

        // We can't traverse the AST, because auto's are already resolved
        unsigned numTokens;
        CXToken *tokens = nullptr;
        clang_tokenize(translationUnit, clang_getCursorExtent(cursor), &tokens, &numTokens);

        std::vector<Replacement> replacements;
        bool prevWasAuto = false;
        for (unsigned i = 0; i < numTokens; i++) {
            CXSourceRange extent = clang_getTokenExtent(translationUnit, tokens[i]);
            const CXSourceLocation start = clang_getRangeStart(extent);
            const CXSourceLocation end = clang_getRangeEnd(extent);
            if (clang_Location_isInSystemHeader(start)) {
                continue;
            }

            if (skipHeaders && !clang_Location_isFromMainFile(start)) {
                continue;
            }

            const std::string tokenString = getString(clang_getTokenSpelling(translationUnit, tokens[i]));

            if (dumpNodes) {
                std::cout << "\n - Token: " << clang_getTokenSpelling(translationUnit, tokens[i]) << std::endl;
                std::cout << " - token kind: " << clang_getTokenKind(tokens[i]) << std::endl;

                CXCursor cursor;
                clang_annotateTokens(translationUnit, &tokens[i], 1, &cursor);
                CXType type = clang_getCursorType(cursor);
                std::cout << " - type " << clang_getTypeSpelling(type) << std::endl;
                std::cout << " - kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
                std::cout << " - cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
                std::cout << " - cursor type kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
            }

            // Handle "const auto" shit pointers
            if (tokenString == "const") {
                CXCursor cursor;
                clang_annotateTokens(translationUnit, &tokens[i], 1, &cursor);
                CXType type = clang_getCursorType(cursor);

                if (type.kind != CXType_Auto) {
                    continue;
                }
                const std::string typeString = getString(clang_getTypeSpelling(type));
                if (typeString.find("const") == 0) { // idk..
                    continue;
                }

                Replacement replacement;
                replacement.string = "";

                clang_getSpellingLocation(start, nullptr, nullptr, nullptr, &replacement.start);
                clang_getSpellingLocation(end, nullptr, nullptr, nullptr, &replacement.end);
                replacements.push_back(replacement);

                continue;
            }

            // Handle auto&& crap
            if (prevWasAuto && clang_getTokenKind(tokens[i]) != CXToken_Keyword && tokenString == "&&") {
                prevWasAuto = false;

                CXCursor cursor;
                clang_annotateTokens(translationUnit, &tokens[i], 1, &cursor);
                if (cursor.kind != CXCursor_VarDecl) {
                    continue;
                }

                CXType type = clang_getCursorType(cursor);
                if (type.kind != CXType_LValueReference) {
                    std::cerr << "TODO" << std::endl;
                    std::cout << " - type " << clang_getTypeSpelling(type) << std::endl;
                    std::cout << " - kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
                    std::cout << " - cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
                    std::cout << " - cursor type kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
                    continue;
                }

                Replacement replacement;
                replacement.string = "&";

                clang_getSpellingLocation(start, nullptr, nullptr, nullptr, &replacement.start);
                clang_getSpellingLocation(end, nullptr, nullptr, nullptr, &replacement.end);
                replacements.push_back(replacement);

                continue;
            }
            prevWasAuto = false;

            if (clang_getTokenKind(tokens[i]) != CXToken_Keyword) {
                continue;
            }

            if (tokenString == "auto") {
                Replacement replacement = handleAutoToken(&tokens[i], translationUnit);
                if (replacement.string.empty()) {
                    continue;
                }
                replacements.push_back(replacement);
                prevWasAuto = true;
            }
        }
        clang_disposeTokens(translationUnit, tokens, numTokens);

        for (size_t i = 0; i < arguments.size(); i++) {
            free(arguments[i]);
        }

        if (dumpNodes) {
            clang_visitChildren(cursor, &Unfuckifier::dumpChild, nullptr);
        }

        if (replacements.empty()) {
            std::cout << "no autos." << std::endl;
            return true;
        }


        return fixFile(sourceFile, replacements);
    }

    static CXChildVisitResult dumpChild(CXCursor cursor, CXCursor parent, CXClientData client_data)
    {
        Unfuckifier *that = reinterpret_cast<Unfuckifier*>(client_data);

        CXSourceLocation loc = clang_getCursorLocation(cursor);
        if (clang_Location_isInSystemHeader(loc)) {
            return CXChildVisit_Continue;
        }
        const CXCursorKind cursorKind = clang_getCursorKind(cursor);

        if (!clang_Location_isFromMainFile(loc)) {
            return CXChildVisit_Recurse;
        }

        CXType type = clang_getCursorType(cursor);
        std::cout << "\ntype " << clang_getTypeSpelling(type) << std::endl;
        std::cout << " - return type " << clang_getTypeSpelling(clang_getResultType(type)) << std::endl;
        std::cout << " - return type kind " << clang_getTypeKindSpelling(clang_getResultType(type).kind) << std::endl;
        std::cout << " - kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
        std::cout << " - cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
        std::cout << " - pretty printed " << clang_getCursorPrettyPrinted(cursor, nullptr) << std::endl;
        std::cout << " - num arguments " << clang_Cursor_getNumArguments(cursor) << std::endl;
        for (int i=0; i< clang_Cursor_getNumArguments(cursor); i++) {
            std::cout << " -> argument "  << clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(cursor, i))) << std::endl;
        }

        CXSourceRange extent = clang_getCursorExtent(cursor);

        const CXSourceLocation start = clang_getRangeStart(extent);
        const CXSourceLocation end = clang_getRangeEnd(extent);
        unsigned lineStart, lineEnd;
        CXFile fileStart, fileEnd;
        clang_getSpellingLocation(start, &fileStart, &lineStart, nullptr, nullptr);
        clang_getSpellingLocation(end, &fileEnd, &lineEnd, nullptr, nullptr);
        std::cout << " - line start " << lineStart << " line end " << lineEnd << std::endl;

        return CXChildVisit_Recurse;
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

        std::cout << "found " << replacements.size() << " autos" << std::endl;

        std::vector<char> buffer;
        int lastPos = 0;

        for (const Replacement &replacement : replacements) {
            //if (replacement.string.empty()) {
            //    if (verbose) std::cout << "Skipping empty replacement" << std::endl;
            //    continue;
            //}
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

    CXCompilationDatabase compilationDatabase{};
    CXIndex index = nullptr;
    CXTranslationUnit translationUnit = nullptr;

    bool replaceFile = false;
    bool verbose = false;
    bool skipBuildDir = true;
    bool dumpNodes = false;
    bool skipHeaders = false;
};

static void printUsage(const std::string &executable)
{
    std::cerr << "Please pass a compile_commands.json and a source file, or --all to fix all files in project" << std::endl;
    std::cerr << "To replace the existing files pass --replace" << std::endl;
    std::cerr << "\t" << executable << " path/to/compile_commands.json [--verbose] [--dump-nodes] [--replace] [--skip-headers] [--stop-on-fail] [--all] [path/to/heretical.cpp]" << std::endl;
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
    bool stopOnFail = false;
    for (int argNum = 1; argNum < argc; argNum++) {
        const std::string arg = argv[argNum];

        if (arg == "--all") {
            all = true;
            continue;
        }
        if (arg == "--replace") {
            fixer.replaceFile = true;
            continue;
        }
        if (arg == "--verbose") {
            fixer.verbose = true;
            continue;
        }
        if (arg == "--dump-nodes") {
            fixer.dumpNodes = true;
            continue;
        }
        if (arg == "--skip-headers") {
            fixer.skipHeaders = true;
            continue;
        }
        if (arg == "--stop-on-fail") {
            stopOnFail = true;
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
    chdir(compileDbPath.c_str());

    fixer.parseCompilationDatabase(compileDbPath.string());

    if (all) {
        for (const std::string &file : fixer.allAvailableFiles()) {
            if (!fixer.process(file)) {
                std::cerr << "Failed to process " << file << std::endl;
                if (!stopOnFail) {
                    return 1;
                }
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
