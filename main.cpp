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
std::ostream& operator<<(std::ostream& stream, const CXString& str)
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
typedef std::string asdf;

static CXChildVisitResult functionVisitor( CXCursor cursor, CXCursor /* parent */, CXClientData /* clientData */ )
{
  if( clang_Location_isFromMainFile( clang_getCursorLocation( cursor ) ) == 0 )
    return CXChildVisit_Continue;

  CXCursorKind kind = clang_getCursorKind( cursor );
  auto name = getString(clang_getCursorSpelling( cursor ));

  if( kind == CXCursorKind::CXCursor_FunctionDecl || kind == CXCursorKind::CXCursor_CXXMethod || kind == CXCursorKind::CXCursor_FunctionTemplate )
  {
    CXSourceRange extent           = clang_getCursorExtent( cursor );
    CXSourceLocation startLocation = clang_getRangeStart( extent );
    CXSourceLocation endLocation   = clang_getRangeEnd( extent );

    unsigned int startLine = 0, startColumn = 0;
    unsigned int endLine   = 0, endColumn   = 0;

    clang_getSpellingLocation( startLocation, nullptr, &startLine, &startColumn, nullptr );
    clang_getSpellingLocation( endLocation,   nullptr, &endLine, &endColumn, nullptr );

    std::cout << "  " << name << ": " << endLine - startLine << "\n";
  }

  return CXChildVisit_Recurse;
}

static        CXTranslationUnit translationUnit;
struct Unfuckifier
{
    bool parseCompileDatabase(const QFileInfo &fileInfo)
    {
        const std::string compileDbFilename = fileInfo.canonicalFilePath().toUtf8().toStdString();
        const std::string compilePath = fileInfo.canonicalPath().toUtf8().toStdString();

        CXCompilationDatabase_Error compilationDatabaseError;
        CXCompilationDatabase compilationDatabase = clang_CompilationDatabase_fromDirectory(
                compilePath.c_str(),
                &compilationDatabaseError
            );

        CXCompileCommands compileCommands = clang_CompilationDatabase_getCompileCommands(
                compilationDatabase,
                "/home/sandsmark/src/unfuckify/main.cpp"
                //compileDbFilename.c_str()
            );
        size_t numCompileCommands = clang_CompileCommands_getSize( compileCommands );

        CXCompileCommand compileCommand = clang_CompileCommands_getCommand( compileCommands, 0 );
        size_t numArguments = clang_CompileCommand_getNumArgs( compileCommand );
        char** arguments = new char*[numArguments];

        for(size_t i = 0; i < numArguments; i++) {
            CXString argument       = clang_CompileCommand_getArg( compileCommand, i );
            std::string strArgument = clang_getCString( argument );
            arguments[i]            = new char[ strArgument.size() + 1 ];

            std::fill( arguments[i],
                    arguments[i] + strArgument.size() + 1,
                    0 );

            std::copy( strArgument.begin(), strArgument.end(),
                    arguments[i] );

            qDebug() << argument;
            clang_disposeString( argument );
        }

        CXIndex index = clang_createIndex(
                0,
                1 // Print warnings and errors
            );
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

        CXCursor rootCursor = clang_getTranslationUnitCursor( translationUnit );
        clang_visitChildren( rootCursor, functionVisitor, this );

        if (translationUnit == nullptr) {
            qWarning() << "Unable to parse translation unit. Quitting.";
            return false;
        }

        //CXCursor cursor = clang_getTranslationUnitCursor(translationUnit);
        auto cursor = clang_getTranslationUnitCursor(translationUnit);
        clang_visitChildren(cursor, &Unfuckifier::visitChild, nullptr);

        for (size_t i = 0; i < numArguments; i++) {
            delete[] arguments[i];
        }

        delete[] arguments;
        clang_disposeIndex(index);

        return true;
    }

    bool parseFile(const std::string &filename)
    {
        CXIndex index = clang_createIndex(0, 0);
        const char *opts[] = {
            "-x", "c++"
        };
        CXTranslationUnit unit = clang_parseTranslationUnit(
                index,
                filename.c_str(), opts, 2,
                nullptr, 0,
                CXTranslationUnit_KeepGoing | CXTranslationUnit_SingleFileParse);

        if (unit == nullptr) {
            qWarning() << "Unable to parse translation unit. Quitting.";
            return false;
        }

        auto cursor = clang_getTranslationUnitCursor(unit);
        //CXCursor cursor = clang_getTranslationUnitCursor(unit);

        clang_visitChildren(cursor, &Unfuckifier::visitChild, nullptr);
        qDebug() << "Parsing complete";

        clang_disposeTranslationUnit(unit);
        clang_disposeIndex(index);

        return true;
    }

    static CXChildVisitResult visitChild(CXCursor c, CXCursor parent, CXClientData client_data)
    {
        Unfuckifier *that = reinterpret_cast<Unfuckifier*>(client_data);

        CXSourceLocation loc = clang_getCursorLocation(c);
        if (clang_Location_isInSystemHeader(loc)) {
            return CXChildVisit_Continue;
        }
        const CXCursorKind cursorKind = clang_getCursorKind(c);

        if (!clang_Location_isFromMainFile(loc)) {
            return CXChildVisit_Recurse;
        }
        CXSourceLocation parentLoc = clang_getCursorLocation(parent);
        if (!clang_Location_isFromMainFile(parentLoc)) {
            return CXChildVisit_Recurse;
        }
//                qDebug() << "\n=======";
//        qDebug() << "cursor type" << clang_getTypeSpelling((clang_getCursorType(c)));
//        qDebug() << "cursor kind" << clang_getTypeKindSpelling((clang_getCursorType(c).kind));
//        qDebug() << "cursor displayname" << clang_getCursorDisplayName(c);
//        qDebug() << "parent display name" << clang_getCursorDisplayName(parent);
//        qDebug() << "parent kind" << clang_getTypeKindSpelling((clang_getCursorType(parent).kind));
//        qDebug() << "parent cursor kind" << clang_getCursorKind(parent);
//
//                qDebug() << "-------\n";

        //qDebug() << "cursor pretty" << clang_getCursorPrettyPrinted(c, nullptr);
        //if (clang_getCursorType(c).kind == CXType_FunctionProto) {
        //        qDebug() << "f- unction proto return type" << clang_getTypeSpelling(clang_getCursorResultType(c));
        //        qDebug() << "cursor type" << clang_getTypeSpelling((clang_getCursorType(c)));
        //        qDebug() << "cursor kind" << clang_getTypeKindSpelling((clang_getCursorType(c).kind));
        //        qDebug() << "cursor displayname" << clang_getCursorDisplayName(c);
        //        qDebug() << "canonical type" << clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(c)));
        //        qDebug() << "-------";
        //}
        
        //if (clang_getCursorType(parent).kind == CXType_Auto) {// && clang_getCursorKind(parent) == CXCursor_CallExpr ) {// && clang_getCursorType(c).kind == CXType_Elaborated) {
        //if (clang_getCursorKind(parent) == CXCursor_VarDecl) {// && clang_getCursorType(c).kind == CXType_Elaborated) {
           // qDebug() << "\nparent:" << clang_getCursorSpelling(parent);
           // qDebug() << clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(parent)));
           // qDebug() << "parent type" << clang_getTypeSpelling((clang_getCursorType(parent)));
           // qDebug() <<  "parent typedef" << clang_getTypedefName((clang_getCursorType(parent)));
           // qDebug() << clang_getCursorKind(parent);
           // qDebug() << "=======";
           // qDebug() << "vardecl:" << clang_getCursorSpelling(c);
           // qDebug() << clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(c)));
           // qDebug() << "cursor type:" << clang_getTypeSpelling((clang_getCursorType(c)));
           // qDebug() << "typedef" << clang_getTypedefName((clang_getCursorType(c)));

            if(clang_getCursorKind(parent) == CXCursor_CallExpr && clang_getCursorType(parent).kind == CXType_Auto) {
                //CXFile file;
                //unsigned int line, column, offset;
                //clang_getInstantiationLocation(startLocation, &file, &line, &column, &offset);
                //printf("Start: Line: %u Column: %u Offset: %u\n", line, column, offset);
                //clang_getInstantiationLocation(endLocation, &file, &line, &column, &offset);
                //printf("End: Line: %u Column: %u Offset: %u\n", line, column, offset);
                auto name = getString(clang_getCursorSpelling( c ));
                CXSourceRange extent           = clang_getCursorExtent( c );
                CXSourceLocation startLocation = clang_getRangeStart( extent );
                CXSourceLocation endLocation   = clang_getRangeEnd( extent );

                unsigned int startLine = 0, startColumn = 0;
                unsigned int endLine   = 0, endColumn   = 0;

                clang_getSpellingLocation( endLocation,   nullptr, &endLine, &endColumn, nullptr );
                clang_getSpellingLocation( startLocation, nullptr, &startLine, &startColumn, nullptr );
                //if (startLine > 0 && (clang_getCursorType(c).kind == CXType_Auto || clang_getCursorType(parent).kind ==CXType_Auto )) {
                    qDebug() << "\n=======";
                    qDebug() << "cursor type" << clang_getTypeSpelling((clang_getCursorType(c)));
                    qDebug() << "cursor kind" << clang_getTypeKindSpelling((clang_getCursorType(c).kind));
                    qDebug() << "cursor displayname" << clang_getCursorDisplayName(c);
                    CXCursor parentparent = clang_getCursorSemanticParent(parent);
                    qDebug() << "parent display name" << clang_getCursorDisplayName(parentparent);
                    qDebug() << "parent kind" << clang_getTypeKindSpelling((clang_getCursorType(parentparent).kind));
                    qDebug() << "parent cursor kind" << clang_getCursorKind(parentparent);

                    qDebug() << "-------\n";


                    std::cout << "  " << name << ": " << endLine << "-" << startLine << " " << startColumn << "\n";
                    {
                        extent           = clang_getCursorExtent( parent );
                        startLocation = clang_getRangeStart( extent );
                        endLocation   = clang_getRangeEnd( extent );
                        clang_getSpellingLocation( endLocation,   nullptr, &endLine, &endColumn, nullptr );
                        clang_getSpellingLocation( startLocation, nullptr, &startLine, &startColumn, nullptr );
                        std::cout << "  " << name << ": " << endLine << "-" << startLine << " " << startColumn << "\n";
                    }
                    {
                        extent           = clang_getCursorExtent( clang_getCursorSemanticParent(parent) );
                        startLocation = clang_getRangeStart( extent );
                        endLocation   = clang_getRangeEnd( extent );
                        clang_getSpellingLocation( endLocation,   nullptr, &endLine, &endColumn, nullptr );
                        clang_getSpellingLocation( startLocation, nullptr, &startLine, &startColumn, nullptr );
                        std::cout << "  " << name << ": " << endLine << "-" << startLine << " " << startColumn << "\n";
                    }
                //}
            }
            //qDebug() << "-----\n";
        //}
        switch (cursorKind){
            case CXType_FunctionProto:
                qDebug() << "function proto return type" << clang_getTypeSpelling(clang_getCursorResultType(c));
                break;
            case CXCursor_DeclStmt:
                //qDebug() << "decl" << clang_getCursorSpelling(c);
                break;
            case CXType_Typedef:
                //qDebug() << "typedef" << clang_getTypedefName(clang_getCursorType(c));
                //if (parent) {
                //qDebug() << "typedef" << clang_getTypedefName(clang_getCursorType(parent));
                //}
                //qDebug() << "cursor type" << clang_getTypeSpelling((clang_getCursorType(c)));
                //qDebug() << "canonical type" << clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(c)));
                //qDebug() << "Args" << clang_Cursor_getNumArguments(c);
                break;
            case CXCursor_VarDecl:
                //if (clang_getCursorType(c) == CXType_Auto) {
                //if (clang_getCursorType(c).kind == CXType_Auto) {
                //    qDebug() << "vardecl:" << clang_getCursorSpelling(c);
                //    qDebug() << clang_getTypeSpelling(clang_getCanonicalType(clang_getCursorType(c)));
                //    qDebug() << clang_getTypeSpelling((clang_getCursorType(c)));
                //    qDebug() << clang_getTypedefName((clang_getCursorType(c)));
                //}
                break;
            case CXCursor_CXXAccessSpecifier:
            case CXCursor_CXXBoolLiteralExpr:
            case CXCursor_Constructor:
            case CXCursor_Destructor:
            case CXCursor_CXXMethod: {
                //CXType returnType = clang_getCursorResultType(c);
                //qDebug() << "return type" << clang_getTypeSpelling(returnType);
                break;
             }
            case CXCursor_ParmDecl:
                break;
            default:
                break;
                return CXChildVisit_Recurse;
        }
        //qDebug() << "-------";
        //if (cursorKind == CXCursor_CXXMethod) {
        //    handleMethod(c, parent);
        //}
        // TODO: if there's more we need to handle
        return CXChildVisit_Recurse;
    }
    //std::unordered_map<CXSourceRange, std::string> m_replacements;
};

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    const QStringList arguments = a.arguments();
    if (arguments.count() < 2) {
        qWarning() << "Please pass a header file";
        return 1;
    }

    QFileInfo fileInfo(argv[1]);
    if (!fileInfo.exists()) {
        qWarning() << fileInfo.fileName() << "does not exist";
        return 1;
    }

    //Mocker mocker(fileInfo.baseName().toStdString());
    Unfuckifier fixer;

    if (fileInfo.fileName() == "compile_commands.json") {
        std::cout << "Using compile_commands" << std::endl;
        if (!fixer.parseCompileDatabase(fileInfo)) {
            std::cerr << "Failed to parse " << argv[1] << std::endl;
            return 1;
        }
    } else {
        //const std::string filename = fileInfo.canonicalFilePath().toUtf8().toStdString();
        //std::cout << "Handling single file " << filename << std::endl;
        //if (!fixer.parseFile(filename)) {
            qWarning() << "Failed to parse" << fileInfo.fileName();
            return 1;
        //}
    }

    return 0;
}
