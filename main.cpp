#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <map>
#include <filesystem>
#include <vector>

#include <cstring>
#include <cassert>

extern "C" {
#include <unistd.h>
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>
}

std::ostream &operator<<(std::ostream &stream, const CXString &str)
{
    const char *cstr = clang_getCString(str);
    if (cstr) {
        stream << cstr;
    } else {
        stream << "(nullptr)";
    }
    clang_disposeString(str);
    return stream;
}

std::string getString(const CXString &str)
{
    const char *cstr = clang_getCString(str);
    std::string ret;
    if (cstr) ret = std::string(cstr);
    else ret = "(nullptr)";
    clang_disposeString(str);
    return ret;
}

namespace std {
template<>
struct hash<CXSourceLocation> {
    size_t operator()(CXSourceLocation const& any) const {
        unsigned line, col;
        clang_getSpellingLocation(any, nullptr, &line, &col, nullptr);
        // very inefficient idk
        return (line ^ col);
    }
};
};

static bool operator==(const CXSourceLocation &a, const CXSourceLocation &b)
{
    return clang_equalLocations(a, b);
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

            return replacement;
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

    void grokFile(const std::string &sourceFile, CXSourceRange extent)
    {
        if (verbose) std::cout << "Checking " << sourceFile << std::endl;

        // We can't traverse the AST, because auto's are already resolved
        unsigned numTokens;
        CXToken *tokens = nullptr;
        clang_tokenize(translationUnit, extent, &tokens, &numTokens);

        std::unordered_map<CXSourceLocation, Replacement> lambdas;
        std::unordered_map<CXSourceLocation, CXSourceLocation> autoLambdas;

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
            CXCursor cursor;
            clang_annotateTokens(translationUnit, &tokens[i], 1, &cursor);

            if (dumpNodes) {
                std::cout << "\n - Token: " << clang_getTokenSpelling(translationUnit, tokens[i]) << std::endl;
                std::cout << " - token kind: " << clang_getTokenKind(tokens[i]) << std::endl;

                CXType type = clang_getCursorType(cursor);
                std::cout << " - type " << clang_getTypeSpelling(type) << std::endl;
                std::cout << " - kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
                std::cout << " - cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
                std::cout << " - cursor type kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
            }

            // Handle "const auto" shit pointers
            if (tokenString == "const") {
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

            if (cursor.kind == CXCursor_LambdaExpr) {
                CXType type = clang_getCursorType(clang_getCursorSemanticParent(cursor));
                if (type.kind != CXType_Auto) {
                    continue;
                }

                Replacement replacement;
                autoLambdas[clang_getCursorLocation(cursor)] = clang_getCursorLocation(clang_getCursorSemanticParent(cursor));
            }

            // Handle auto&& crap
            if (prevWasAuto && clang_getTokenKind(tokens[i]) != CXToken_Keyword && tokenString == "&&") {
                prevWasAuto = false;

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
                //continue;
            }

            if (tokenString == "auto") {
                prevWasAuto = true;
                Replacement replacement = handleAutoToken(&tokens[i], translationUnit);
                if (replacement.string.empty()) {
                    continue;
                }
                if (replacement.string.find("(lambda at ") != std::string::npos) {
                    lambdas[clang_getCursorLocation(cursor)] = replacement;
                } else {
                    replacements.push_back(replacement);
                }
                continue;
            }

            // For some reason we have to keep checking the parents, doesn't seem like stuff is resolved otherwise
            CXCursor parent = clang_getCursorSemanticParent(cursor);
            CXType parentType = clang_getCursorType(parent);
            if (parentType.kind == CXType_FunctionProto) {
                if (clang_getResultType(parentType).kind != CXType_Auto) {
                    continue;
                }

                std::unordered_map<CXSourceLocation, CXSourceLocation>::const_iterator firstIt = autoLambdas.find(clang_getCursorLocation(parent));
                if (firstIt == autoLambdas.end()) {
                    //std::cerr << "Failed to find lambda auto location" << std::endl;
                    //dumpCursor(parent);
                    continue;
                }

                std::unordered_map<CXSourceLocation, Replacement>::const_iterator it = lambdas.find(firstIt->second);
                if (it == lambdas.end()) {
                    std::cerr << "Failed to find lambda" << std::endl;
                    dumpCursor(parent);
                    continue;
                }
                Replacement replacement = it->second;
                replacement.string = getString(clang_getTypeSpelling(parentType));
                std::string::size_type constStart = replacement.string.find(" const");
                if (constStart == replacement.string.size() - strlen(" const")) {
                    replacement.string = replacement.string.substr(0, replacement.string.size() - strlen(" const"));
                }
                replacement.string = "std::function<" + replacement.string + ">";
                if (verbose) std::cout << "Found lambda: " << replacement.start << "-" << replacement.end << std::endl;
                if (verbose) std::cout << replacement.string << std::endl;
                replacements.push_back(replacement);
            }
        }
        clang_disposeTokens(translationUnit, tokens, numTokens);

        if (replacements.empty()) {
            std::cout << sourceFile << " no autos." << std::endl;
            return;
        }


        fixFile(sourceFile, replacements);
    }

    static void inclusionVisitor(CXFile included_file, CXSourceLocation* inclusion_stack, unsigned include_len, CXClientData client_data)
    {
        Unfuckifier *that = reinterpret_cast<Unfuckifier*>(client_data);
        if (include_len == 0) {
            return;
        }

        CXSourceLocation startLocation = clang_getLocationForOffset(that->translationUnit, included_file, 0);

        if (clang_Location_isInSystemHeader(startLocation)) {
            return;
        }

        // stl is utter crap, as usual, because nice APIs would be too much
        std::error_code fsError;
        std::string filePath = std::filesystem::canonical(getString(clang_getFileName(included_file)), fsError).string();
        if (fsError) {
            std::cerr << "Failed to find proper name for " << getString(clang_getFileName(included_file)) << ": " << fsError.message() << std::endl;
            return;
        }

        // libclang is shit, clang_Location_isInSystemHeader works some times, aka. never
        if (filePath.find("/usr/") == 0) {
            return;
        }

        if (that->parsedFiles.count(filePath)) {
            return;
        }
        that->parsedFiles.insert(filePath);

        size_t fileSize;
        const char *fileContents = clang_getFileContents(that->translationUnit, included_file, &fileSize);
        if (!fileContents || fileSize == 0) {
            return;
        }

        if (!memmem(fileContents, fileSize, "auto", strlen("auto"))) {
            if (that->verbose) std::cout << filePath << " doesn't contain auto" << std::endl;
            return;
        }

        const CXSourceLocation endLocation = clang_getLocationForOffset(that->translationUnit, included_file, fileSize);
        that->grokFile(filePath, clang_getRange(startLocation, endLocation));
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

        if (index) {
            clang_disposeIndex(index);
        }

        index = clang_createIndex(
                            0,
                            1 // Print warnings and errors
                        );

        if (translationUnit) {
            clang_disposeTranslationUnit(translationUnit);
        }

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
        if (verbose) std::cout << "Finished parsing" << std::endl;

        for (size_t i = 0; i < arguments.size(); i++) {
            free(arguments[i]);
        }

        for (unsigned i=0; i<clang_getNumDiagnostics(translationUnit); i++) {
            CXDiagnostic diagnostic = clang_getDiagnostic(translationUnit, i);
            const CXDiagnosticSeverity severity = clang_getDiagnosticSeverity(diagnostic);
            clang_disposeDiagnostic(diagnostic);

            if (severity >= CXDiagnostic_Error) {
                std::cout << "Compile error" << std::endl;
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

        if (!skipHeaders) {
            clang_getInclusions(translationUnit, inclusionVisitor, this);
        }

        // auto for testing
        auto cursor = clang_getTranslationUnitCursor(translationUnit);
        grokFile(sourceFile, clang_getCursorExtent(cursor));

        if (dumpNodes) {
            clang_visitChildren(cursor, &Unfuckifier::dumpChild, nullptr);
        }

        return true;
    }

    static void dumpCursor(CXCursor cursor)
    {
        std::cout << "-------------" << std::endl;

        CXType type = clang_getCursorType(cursor);
        std::cout << " - type " << clang_getTypeSpelling(type) << std::endl;
        std::cout << " - return type " << clang_getTypeSpelling(clang_getResultType(type)) << std::endl;
        std::cout << " - return type kind " << clang_getTypeKindSpelling(clang_getResultType(type).kind) << std::endl;
        std::cout << " - kind " << clang_getTypeKindSpelling(type.kind) << std::endl;
        std::cout << " - cursor kind " << clang_getCursorKindSpelling(cursor.kind) << std::endl;
        std::cout << " - pretty printed " << clang_getCursorPrettyPrinted(cursor, nullptr) << std::endl;
        std::cout << " - num t arguments " << clang_Cursor_getNumTemplateArguments(cursor) << std::endl;
        std::cout << " - num arguments " << clang_Cursor_getNumArguments(cursor) << std::endl;
        for (int i=0; i< clang_Cursor_getNumArguments(cursor); i++) {
            std::cout << " -> argument "  << clang_getTypeSpelling(clang_getCursorType(clang_Cursor_getArgument(cursor, i))) << std::endl;
        }
        CXSourceRange extent = clang_getCursorExtent(cursor);

        const CXSourceLocation start = clang_getRangeStart(extent);
        const CXSourceLocation end = clang_getRangeEnd(extent);
        unsigned lineStart, lineEnd;
        unsigned colStart, colEnd;
        CXFile fileStart, fileEnd;
        clang_getSpellingLocation(start, &fileStart, &lineStart, &colStart, nullptr);
        clang_getSpellingLocation(end, &fileEnd, &lineEnd, &colEnd, nullptr);
        std::cout << " - line start " << lineStart << " line end " << lineEnd << std::endl;
        std::cout << " - col start " << colStart << " col end " << colEnd << std::endl;
        std::cout << "====" << std::endl;
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

        std::cout << "\ncursor" << std::endl;
        dumpCursor(cursor);
        std::cout << "\nparent" << std::endl;
        dumpCursor(parent);
        //std::cout << "\nparent parent" << std::endl;
        //dumpCursor(clang_getCursorSemanticParent(parent));
        //std::cout << "\nparent parent parent" << std::endl;
        //dumpCursor(clang_getCursorSemanticParent(clang_getCursorSemanticParent(parent)));
        std::cout << std::endl;

        return CXChildVisit_Recurse;
    }


    bool fixFile(const std::string &filePath, std::vector<Replacement> &replacements)
    {
        std::sort(replacements.begin(), replacements.end(), [](const Replacement &a, const Replacement &b) {
            return a.start > b.start;
        });
        // Meh, could check when we add them, but I'm lazy
        replacements.erase(std::unique(replacements.begin(), replacements.end(), [](const Replacement &a, const Replacement &b) {
            return a.start == b.start && a.end == b.end && a.string == b.string;
        }), replacements.end());
        for (const Replacement &r : replacements) {
            if (verbose) std::cout << "Replacement: " << r.start << " - " << r.end << " -> " << r.string << std::endl;
        }
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

        bool failed = false;
        for (const Replacement &replacement : replacements) {
            //if (replacement.string.empty()) {
            //    if (verbose) std::cout << "Skipping empty replacement" << std::endl;
            //    continue;
            //}
            buffer.resize(replacement.start - lastPos);
            if (fread(buffer.data(), buffer.size(), 1, file) != 1) {
                std::cerr << "Failed to read from original file" << std::endl;
                failed = true;
                break;
            }
            if  (fwrite(buffer.data(), buffer.size(), 1, outFile) != 1) {
                std::cerr << "Failed to write to file" << std::endl;
                failed = true;
                break;
            }
            if (fwrite(replacement.string.c_str(), replacement.string.size(), 1, outFile) != 1) {
                std::cerr << "Failed to write replacement to file" << std::endl;
                failed = true;
                break;
            }

            lastPos = replacement.end;
            fseek(file, replacement.end - replacement.start, SEEK_CUR);
        }

        if (failed) {
            fclose(file);
            fclose(outFile);
            return false;
        }

        if (lastPos < fileSize) {
            buffer.resize(fileSize - lastPos);
            if (fread(buffer.data(), buffer.size(), 1, file) != 1) {
                std::cerr << "Failed to read remainder" << std::endl;
                fclose(file);
                fclose(outFile);
                return false;
            }

            if (fwrite(buffer.data(), buffer.size(), 1, outFile) != 1) {
                std::cerr << "Failed to write remainder" << std::endl;
                fclose(file);
                fclose(outFile);
                return false;
            }
        }

        fclose(file);
        fclose(outFile);

        if (failed) {
            return false;
        }

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
    std::unordered_set<std::string> parsedFiles;
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
    int posixSucks = chdir(compileDbPath.c_str());
    if (posixSucks) {
        std::cerr << "failed to chdir to " << compileDbPath << ", noone cares" << std::endl;
    }

    fixer.parseCompilationDatabase(compileDbPath.string());

    if (all) {
        const std::vector<std::string> files = fixer.allAvailableFiles();
        for (size_t i=0; i<files.size(); i++) {
            std::cout << i << "/" << files.size() << std::endl;
            if (!fixer.process(files[i])) {
                std::cerr << "Failed to process " << files[i] << std::endl;
                if (stopOnFail) {
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
    std::cout << "Done!" << std::endl;

    return 0;
}
