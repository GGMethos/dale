#include "Reader.h"
#include "Config.h"

#include "../../llvm_LLVMContext.h"
#include "../../llvm_Module.h"
#include "../../llvm_Linker.h"
#include "../../llvm_Function.h"
#include "../../llvm_CallingConv.h"
#include "../../llvm_AssemblyPrintModulePass.h"
#include "../../llvm_ValueSymbolTable.h"
#include "../../llvm_AnalysisVerifier.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/JIT.h"
#include "llvm/ExecutionEngine/Interpreter.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"

#if D_LLVM_VERSION_MINOR <= 4
#include "llvm/Support/system_error.h"
#else
#include "llvm/Object/Error.h"
#endif
#if D_LLVM_VERSION_MINOR >= 3
#include "llvm/Support/SourceMgr.h"
#include "llvm/IRReader/IRReader.h"
#endif

#include "../../Serialise/Serialise.h"
#include "../../Utils/Utils.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace dale::ErrorInst;

static const char *bc_suffix    = ".bc";
static const char *bc_nm_suffix = "-nomacros.bc";
static const char *so_suffix    = ".so";

namespace dale
{
namespace Module
{
Reader::Reader(std::vector<const char *> *module_directory_paths,
               std::vector<std::string> *so_paths,
               std::vector<const char *> *include_directory_paths)
{
    cwd = getcwd(NULL, 0);
    this->module_directory_paths.push_back(cwd);
    std::copy(module_directory_paths->begin(),
              module_directory_paths->end(),
              back_inserter(this->module_directory_paths));
    this->module_directory_paths.push_back(DALE_MODULE_PATH);

    this->include_directory_paths.push_back("");
    this->include_directory_paths.push_back("./include/");
    std::copy(include_directory_paths->begin(),
              include_directory_paths->end(),
              back_inserter(this->include_directory_paths));
    std::string standard_include_path(DALE_INCLUDE_PATH);
    standard_include_path.append("/");
    this->include_directory_paths.push_back(standard_include_path.c_str());

    this->so_paths = so_paths;
}

Reader::~Reader()
{
    free(cwd);
}

llvm::Module *
Reader::loadModule(std::string *path)
{
#if D_LLVM_VERSION_MINOR <= 4
    llvm::OwningPtr<llvm::MemoryBuffer> buffer;
#endif

#if D_LLVM_VERSION_MINOR <= 3
    const llvm::sys::Path sys_path(*path);
    llvm::MemoryBuffer::getFileOrSTDIN(sys_path.c_str(), buffer);
#elif D_LLVM_VERSION_MINOR <= 4
    llvm::MemoryBuffer::getFileOrSTDIN(*path, buffer);
#else
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> eo_buffer =
        llvm::MemoryBuffer::getFileOrSTDIN(*path);
    assert(!eo_buffer.getError() && "cannot load module");
    std::unique_ptr<llvm::MemoryBuffer> buffer = std::move(eo_buffer.get());
#endif

#if D_LLVM_VERSION_MINOR <= 4
    std::string error_msg;
    llvm::Module *module =
        llvm::getLazyBitcodeModule(buffer.get(),
                                   llvm::getGlobalContext(),
                                   &error_msg);
#else
    std::string error_msg;
    llvm::ErrorOr<llvm::Module *> eo_module =
        llvm::getLazyBitcodeModule(buffer.get(),
                                   llvm::getGlobalContext());
    llvm::Module *module = eo_module.get();
    if (!module) {
        error_msg = eo_module.getError().message();
    }
    buffer.release();
#endif

    assert(module && "cannot load module");

#if D_LLVM_VERSION_MINOR <= 4
    bool materialized = module->MaterializeAll(&error_msg);
#else
    std::error_code ec = module->materializeAllPermanently();
    bool materialized = (bool) ec;
    if (ec) {
        error_msg = ec.message();
    }
#endif
    assert(!materialized && "failed to materialize module");
    _unused(materialized);

    return module;
}

bool
Reader::addDynamicLibrary(const char *path, bool add_to_so_paths,
                          bool add_nm_to_so_paths)
{
    std::string error_msg;
    bool res =
        llvm::sys::DynamicLibrary::LoadLibraryPermanently(
            path,
            &error_msg
        );

    if (res) {
        fprintf(stderr, "%s\n", error_msg.c_str());
        error("unable to load dynamic library", false);
        return false;
    }

    if (add_nm_to_so_paths) {
        std::string nm_path;
        nm_path.append(path);
        nm_path.erase((nm_path.size() - 3), 3);
        nm_path.append("-nomacros.so");
        so_paths->push_back(nm_path);
    } else if (add_to_so_paths) {
        std::string m_path;
        m_path.append(path);
        so_paths->push_back(m_path);
    }

    return true;
}

bool
removeUnneededForms(Context *ctx, Context *new_ctx,
                    std::string *lib_module_name, Node *n,
                    std::vector<const char *> *import_forms)
{
    if (import_forms->size() == 0) {
        return true;
    }

    std::set<std::string> forms_set;
    for (std::vector<const char*>::iterator b = import_forms->begin(),
                                            e = import_forms->end();
            b != e;
            ++b) {
        forms_set.insert(std::string(*b));
    }

    std::set<std::string> found;
    new_ctx->removeUnneeded(&forms_set, &found);

    std::set<std::string> not_found;
    set_difference(forms_set.begin(), forms_set.end(),
                   found.begin(),     found.end(),
                   std::insert_iterator<std::set<std::string> >(
                       not_found,
                       not_found.end()
                   ));
    if (not_found.size() > 0) {
        std::string missing_forms;
        for (std::set<std::string>::iterator b = not_found.begin(),
                                                e = not_found.end();
                b != e;
                ++b) {
            missing_forms.append(*b).append(", ");
        }
        missing_forms.erase(missing_forms.size() - 2,
                            missing_forms.size() - 1);
        std::string bare_mod_name(*lib_module_name);
        bare_mod_name.replace(0, 3, "");
        Error *e = new Error(ModuleDoesNotProvideForms, n,
                             bare_mod_name.c_str(),
                             missing_forms.c_str());
        ctx->er->addError(e);
        return false;
    }

    return true;
}

void
readFile(FILE *fh, char **buf_ptr)
{
    int fd = fileno(fh);
    struct stat buf;
    int fstat_res = fstat(fd, &buf);
    assert(!fstat_res && "unable to fstat file");
    _unused(fstat_res);

    int size = buf.st_size;
    char *data = (char*) malloc(size);
    if (!data) {
        error("unable to allocate memory", true);
    }
    size_t res = fread(data, 1, size, fh);
    assert((res == (size_t) size) && "unable to read module file");
    _unused(res);

    *buf_ptr = data;
}

bool
Reader::findModule(Context *ctx, Node *n, std::string *lib_module_name,
                   FILE **fh, std::string *prefix)
{
    std::string dtm_path;

    for (std::vector<const char *>::iterator b = module_directory_paths.begin(),
                                             e = module_directory_paths.end();
            b != e;
            ++b) {
        bool append_slash = (*b)[strlen(*b) - 1] != '/';
        prefix->clear();
        prefix->append(*b).append(append_slash ? "/" : "");
        dtm_path.clear();
        dtm_path.append(*prefix).append(*lib_module_name).append(".dtm");
        *fh = fopen(dtm_path.c_str(), "r");
        if (*fh) {
            break;
        }
    }
    if (!*fh) {
        Error *e = new Error(FileError, n, dtm_path.c_str(), strerror(errno));
        ctx->er->addError(e);
        return false;
    }

    return true;
}

bool
Reader::run(Context *ctx, llvm::Module *mod, Node *n, const char *module_name,
            std::vector<const char*> *import_forms)
{
    std::vector<const char *> empty_forms;
    if (import_forms == NULL) {
        import_forms = &empty_forms;
    }

    std::string lib_module_name;
    if (!(strstr(module_name, "lib") == module_name)) {
        lib_module_name.append("lib");
    }
    lib_module_name.append(module_name);

    if (included_modules.find(lib_module_name) != included_modules.end()) {
        return true;
    }

    FILE *fh;
    std::string prefix;
    bool res = findModule(ctx, n, &lib_module_name, &fh, &prefix);
    if (!res) {
        return false;
    }

    std::string bc_path;
    std::string so_path;
    bc_path.append(prefix).append(lib_module_name).append(bc_suffix);
    so_path.append(prefix).append(lib_module_name).append(so_suffix);

    Context *new_ctx = new Context(ctx->er, ctx->nt, ctx->tr);
    char *original_data;
    readFile(fh, &original_data);
    char *data = original_data;

    std::set<std::string> once_tags;
    std::set<std::string> dependencies;
    std::map<std::string, std::string> typemap;
    int cto;

    data = deserialise(ctx->tr, data, new_ctx);
    data = deserialise(ctx->tr, data, &once_tags);
    data = deserialise(ctx->tr, data, &dependencies);
    data = deserialise(ctx->tr, data, &cto);
    data = deserialise(ctx->tr, data, &typemap);
    free(original_data);

    for (std::map<std::string, std::string>::iterator b = typemap.begin(),
                                                      e = typemap.end();
            b != e;
            ++b) {
        std::string from = (*b).first;
        std::string to   = (*b).second;
        addTypeMapEntry(from.c_str(), to.c_str());
    }

    std::string module_path(bc_path);
    std::string module_path_nomacros(bc_path);

    module_path_nomacros.replace(module_path_nomacros.find(".bc"),
                                 3, bc_nm_suffix);

    llvm::Module *new_module = loadModule(&module_path);

    included_modules.insert(lib_module_name);

    for (std::set<std::string>::iterator b = dependencies.begin(),
                                         e = dependencies.end();
            b != e;
            ++b) {
        bool res = run(ctx, mod, n, (*b).c_str(), NULL);
        if (!res) {
            return false;
        }
    }

    if (cto) {
        cto_module_names.insert(lib_module_name);
    }

    int add_to_so_paths =
        (cto_module_names.find(std::string(lib_module_name)) ==
         cto_module_names.end());

    res = addDynamicLibrary(so_path.c_str(), false, add_to_so_paths);
    assert(res && "unable to add library");

    std::set<std::string> all_once_tags;
    std::set_union(included_once_tags.begin(), included_once_tags.end(),
                   once_tags.begin(),          once_tags.end(),
                   std::insert_iterator<std::set<std::string> >(
                       all_once_tags,
                       all_once_tags.end()
                   ));
    new_ctx->eraseOnceForms(&all_once_tags, new_module);

    included_once_tags.clear();
    std::copy(all_once_tags.begin(), all_once_tags.end(),
              std::insert_iterator<std::set<std::string> >(
                  included_once_tags,
                  included_once_tags.end()
              ));

    included_modules.insert(lib_module_name);

    dtm_modules.insert(std::pair<std::string, llvm::Module *>(
                            std::string(lib_module_name),
                            new_module
                       ));
    dtm_nm_modules.insert(std::pair<std::string, std::string>(
                               std::string(lib_module_name),
                               module_path_nomacros
                          ));

    res = removeUnneededForms(ctx, new_ctx, &lib_module_name, n,
                              import_forms);
    if (!res) {
        return false;
    }
    ctx->merge(new_ctx);
    ctx->regetPointersForNewModule(mod);
    ctx->relink();

    return true;
}
}
}
