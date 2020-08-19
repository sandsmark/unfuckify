#include <QCoreApplication>
#include <QDebug>
#include <QByteArray>
#include <QFileInfo>
#include <iostream>
#include <fstream>
#include <unordered_set>
#include <unordered_map>
#include <map>

extern "C" {
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>
}

inline QDebug operator<<(QDebug debug, const CXString &str)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << clang_getCString(str);
    clang_disposeString(str);

    return debug;
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
    bool parseCompileDatabase(const QFileInfo &fileInfo, const std::string &sourceFile)
    {
        const std::string compilePath = fileInfo.canonicalPath().toUtf8().toStdString();

        CXCompilationDatabase_Error compilationDatabaseError;
        CXCompilationDatabase compilationDatabase = clang_CompilationDatabase_fromDirectory(
                    compilePath.c_str(),
                    &compilationDatabaseError
                );

        CXCompileCommands compileCommands = clang_CompilationDatabase_getCompileCommands(
                                                compilationDatabase,
                                                sourceFile.c_str()
                                            );
        size_t numCompileCommands = clang_CompileCommands_getSize(compileCommands);

        CXCompileCommand compileCommand = clang_CompileCommands_getCommand(compileCommands, 0);
        size_t numArguments = clang_CompileCommand_getNumArgs(compileCommand);
        char **arguments = new char *[numArguments];

        for (size_t i = 0; i < numArguments; i++) {
            CXString argument = clang_CompileCommand_getArg(compileCommand, i);
            std::string strArgument = clang_getCString(argument);
            arguments[i] = new char[ strArgument.size() + 1 ];
            std::fill(arguments[i], arguments[i] + strArgument.size() + 1, 0);

            std::copy(strArgument.begin(), strArgument.end(), arguments[i]);

            clang_disposeString(argument);
        }

        CXIndex index = clang_createIndex(
                            0,
                            1 // Print warnings and errors
                        );

        // Need to use the full argv thing for clang to pick up default paths
        clang_parseTranslationUnit2FullArgv(
            index,
            0,
            arguments,
            numArguments,
            nullptr,
            0,
            CXTranslationUnit_None, // flags
            &translationUnit
        );


        if (translationUnit == nullptr) {
            qWarning() << "Unable to parse translation unit. Quitting.";
            return false;
        }

        // auto for testing
        auto cursor = clang_getTranslationUnitCursor(translationUnit);
        clang_visitChildren(cursor, &Unfuckifier::visitChild, this);

        for (size_t i = 0; i < numArguments; i++) {
            delete[] arguments[i];
        }

        delete[] arguments;
        clang_disposeIndex(index);

        return fixFile("/home/sandsmark/src/unfuckify/main.cpp");
    }

    bool fixFile(const std::string &filePath)
    {
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

    static CXChildVisitResult visitChild(CXCursor cursor, CXCursor parent, CXClientData client_data)
    {
        Unfuckifier *that = reinterpret_cast<Unfuckifier *>(client_data);

        CXSourceLocation loc = clang_getCursorLocation(cursor);

        if (clang_Location_isInSystemHeader(loc)) {
            return CXChildVisit_Continue;
        }

        if (!clang_Location_isFromMainFile(loc)) {
            return CXChildVisit_Continue;
        }

        // TODO: this just picks up `auto foo = bar()`, to handle `auto foo = 0`
        // we need to drop the parent-parent stuff I think
        if (clang_getCursorKind(parent) == CXCursor_CallExpr && clang_getCursorType(parent).kind == CXType_Auto) {
            unsigned numTokens;
            CXToken *tokens = nullptr;
            clang_tokenize(that->translationUnit, clang_getCursorExtent(clang_getCursorSemanticParent(parent)), &tokens, &numTokens);

            CXToken *autoToken = nullptr;

            for (unsigned i = 0; i < numTokens; i++) {
                if (clang_getTokenKind(tokens[i]) != CXToken_Keyword) {
                    continue;
                }

                if (getString(clang_getTokenSpelling(that->translationUnit, tokens[i])) == "auto") {
                    autoToken = &tokens[i];
                    break;
                }
            }

            if (!autoToken) {
                clang_disposeTokens(that->translationUnit, tokens, numTokens);
                std::cerr << "Failed to find token!" << std::endl;
                return CXChildVisit_Recurse;
            }

            Replacement replacement;
            replacement.string = getString(clang_getTypeSpelling((clang_getCursorType(parent))));
            CXSourceRange extent = clang_getTokenExtent(that->translationUnit, *autoToken);
            clang_disposeTokens(that->translationUnit, tokens, numTokens);

            const CXSourceLocation start = clang_getRangeStart(extent);
            const CXSourceLocation end = clang_getRangeEnd(extent);
            clang_getSpellingLocation(start, nullptr, nullptr, nullptr, &replacement.start);
            clang_getSpellingLocation(end, nullptr, nullptr, nullptr, &replacement.end);

            that->replacements.push_back(std::move(replacement));
            return CXChildVisit_Recurse;
        }

        return CXChildVisit_Recurse;
    }

    struct Replacement {
        unsigned start, end;
        std::string string;
    };
    std::vector<Replacement> replacements;

    CXTranslationUnit translationUnit;
};

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    const QStringList arguments = a.arguments();

    if (arguments.count() < 3) {
        qWarning() << "Please pass a compile_commands.json and a source file to fix";
        return 1;
    }

    QFileInfo fileInfo(argv[1]);

    if (!fileInfo.exists() || !QFileInfo::exists(argv[2])) {
    qWarning() << "files do not exist";
        return 1;
    }

    //Mocker mocker(fileInfo.baseName().toStdString());
    Unfuckifier fixer;

    if (fileInfo.fileName() == "compile_commands.json") {
    std::cout << "Using compile_commands" << std::endl;

    if (!fixer.parseCompileDatabase(fileInfo, argv[2])) {
            std::cerr << "Failed to parse " << argv[1] << std::endl;
            return 1;
        }
    } else {
        qWarning() << "invalid name";
        return 1;
    }

    return 0;
}
