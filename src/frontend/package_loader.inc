/* Package loading uses the root source arena for graph ownership. Source ASTs
 * retain their own tokens and locations; packages share semantic scopes. */
typedef struct {
    AstProgram *root;
    const FrontendOptions *options;
    DmmPackage *stack[128];
    size_t depth;
} PackageLoader;

static void package_error(const AstProgram *file,size_t token,int code,const char *format,...) {
    char message[4096]; va_list args; va_start(args,format);
    (void)vsnprintf(message,sizeof(message),format,args); va_end(args);
    const AstToken *at=ast_program_token(file,token);
    error_report(global_error_handler,SEVERITY_ERROR,at ? at->span.begin.line : 0,
        at ? at->span.begin.column : 0,ERROR_CATEGORY_SEMANTIC,code,file->source_path,"%s",message);
}

static int valid_module_path(const char *path) {
    if (!path || !*path || strlen(path) > 1024) return 0;
    const char *part=path;
    for (const char *p=path;;p++) {
        if (*p == '/' || !*p) {
            size_t n=(size_t)(p-part);
            if (!n || (n == 1 && part[0] == '.') || (n == 2 && !memcmp(part,"..",2))) return 0;
            if (!*p) break;
            part=p+1;
        } else if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return 0;
    }
    return 1;
}

static int valid_dependency_version(const char *version) {
    if (*version++ != 'v') return 0;
    for (int component=0;component<3;component++) {
        if (!isdigit((unsigned char)*version)) return 0;
        while (isdigit((unsigned char)*version)) version++;
        if (component<2 && *version++ != '.') return 0;
    }
    return !*version;
}
static int package_path_prefix(const char *path,const char *prefix) {
    size_t n=strlen(prefix);
    return !strncmp(path,prefix,n) && (path[n] == '\0' || path[n] == '/');
}

static DmmModule *read_module(PackageLoader *loader,const char *directory) {
    char *manifest=joined_path(directory,"dmm.manifest");
    if (!manifest) return NULL;
    FILE *input=fopen(manifest,"rb");
    if (!input) { package_error(loader->root,AST_TOKEN_NONE,ERR_MODULE_MANIFEST,"Missing module manifest '%s'",manifest); free(manifest); return NULL; }
    if (!record_loaded_source(loader->root,manifest)) { fclose(input); free(manifest); return NULL; }
    DmmModule *module=ast_program_alloc(loader->root,sizeof(*module));
    if (!module) { fclose(input); free(manifest); return NULL; }
    module->graph=loader->root->module ? loader->root->module : module;
    module->root=string_interner_intern(loader->root->strings,directory);
    char line[4096]; int group=0,failed=0;
    while (fgets(line,sizeof(line),input)) {
        char *comment=strstr(line,"//");
        int indirect=0;
        if (comment) {
            char marker[32]={0},extra_marker[2]={0};
            indirect=sscanf(comment+2,"%31s %1s",marker,extra_marker) == 1 && !strcmp(marker,"indirect");
            *comment='\0';
        }
        char one[1100]={0},two[1100]={0},three[1100]={0},extra[2]={0};
        int count=sscanf(line,"%1099s %1099s %1099s %1s",one,two,three,extra);
        if (count <= 0) continue;
        if (group && count == 1 && !strcmp(one,")")) { group=0; continue; }
        if (!group && count == 2 && !strcmp(one,"module") && !module->path && valid_module_path(two))
            module->path=string_interner_intern(loader->root->strings,two);
        else if (!group && count == 2 && !strcmp(one,"dmm") && !module->version) {
            if (strcmp(two,"0.3")) { package_error(loader->root,AST_TOKEN_NONE,ERR_MODULE_VERSION,"Manifest '%s' requires DMM %s; this compiler supports DMM 0.3",manifest,two); failed=1; break; }
            module->version=string_interner_intern(loader->root->strings,two);
        } else if (!group && count == 2 && !strcmp(one,"require") && !strcmp(two,"(")) group=1;
        else if (group || (!strcmp(one,"require") && count == 3)) {
            const char *path=group ? one : two, *version=group ? two : three;
            if ((group && count != 2) || !valid_module_path(path) || !valid_dependency_version(version)) { failed=1; break; }
            for (DmmDependency *d=module->dependencies; d; d=d->next) if (!strcmp(d->path,path)) failed=1;
            if (failed) break;
            DmmDependency *dependency=ast_program_alloc(loader->root,sizeof(*dependency));
            if (!dependency) { failed=1; break; }
            dependency->path=string_interner_intern(loader->root->strings,path);
            dependency->version=string_interner_intern(loader->root->strings,version);
            dependency->is_indirect=indirect;
            dependency->next=module->dependencies; module->dependencies=dependency;
        } else { failed=1; break; }
    }
    if (ferror(input) || group || !module->path || !module->version || !module->root) failed=1;
    fclose(input);
    if (failed) package_error(loader->root,AST_TOKEN_NONE,ERR_MODULE_MANIFEST,"Invalid dmm.manifest '%s': expected one valid module path, 'dmm 0.3', and versioned require entries",manifest);
    free(manifest); return failed ? NULL : module;
}

static char *find_module_root(const char *file) {
    char *directory=source_directory(file);
    if (!directory) return NULL;
    while (*directory) {
        char *manifest=joined_path(directory,"dmm.manifest");
        if (!manifest) break;
        struct stat status; int found=stat(manifest,&status) == 0;
        free(manifest); if (found) return directory;
        char *parent=source_directory(directory);
        if (!parent || !strcmp(parent,directory)) { free(parent); break; }
        free(directory); directory=parent;
    }
    free(directory); return NULL;
}

static int string_order(const void *a,const void *b) { return strcmp(*(const char *const *)a,*(const char *const *)b); }

static int attach_package_file(PackageLoader *loader,DmmPackage *package,AstProgram *file) {
    if (!file->structured_ast_complete || !file->package_name) return 0;
    if (package->name && strcmp(package->name,file->package_name)) {
        package_error(file,file->package_token,ERR_PACKAGE_NAME,"Package directory '%s' mixes package '%s' with '%s'",package->directory,package->name,file->package_name); return 0;
    }
    package->name=file->package_name; file->package=package; file->module=package->module;
    file->module_identity=copy_string(package->path);
    AstProgram **files=ast_program_alloc(loader->root,(package->file_count+1)*sizeof(*files));
    if (!files || !file->module_identity) return 0;
    if (package->file_count) memcpy(files,package->files,package->file_count*sizeof(*files));
    files[package->file_count++]=file; package->files=files; return 1;
}

static DmmPackage *load_package(PackageLoader *loader,DmmModule *module,const char *path,const char *directory);

static DmmPackage *resolve_package(PackageLoader *loader,AstProgram *file,const char *path,size_t token) {
    DmmModule *module=file->module;
    if (!valid_module_path(path) || strstr(path,".dmm")) {
        package_error(file,token,ERR_PACKAGE_NOT_FOUND,"Invalid package import '%s'; imports identify packages, never files",path); return NULL;
    }
    if (!package_path_prefix(path,module->path)) {
        if (package_path_prefix(path,"stdlib")) {
            char *bundled=joined_path(DMM_SOURCE_ROOT,"stdlib");
            module=bundled ? read_module(loader,bundled) : NULL; free(bundled);
        } else {
            DmmDependency *chosen=NULL;
            for (DmmDependency *d=module->dependencies; d; d=d->next)
                if (package_path_prefix(path,d->path) && (!chosen || strlen(d->path) > strlen(chosen->path))) chosen=d;
            if (!chosen) { package_error(file,token,ERR_MODULE_DEPENDENCY,"No require entry provides package '%s'",path); return NULL; }
            const char *vendor_root=loader->root->module->root;
            char *vendor=joined_path(vendor_root,"vendor"), *dependency=vendor ? joined_path(vendor,chosen->path) : NULL;
            free(vendor);
            if (!dependency || !path_is_directory(dependency)) { package_error(file,token,ERR_MODULE_DEPENDENCY,"Dependency %s %s is missing from vendor",chosen->path,chosen->version); free(dependency); return NULL; }
            char *canonical=canonical_existing_path(dependency); free(dependency);
            if (!canonical || !path_prefix(canonical,vendor_root)) { package_error(file,token,ERR_COMP_IMPORT_OUTSIDE_ROOT,"Vendored dependency escapes the module root"); free(canonical); return NULL; }
            module=read_module(loader,canonical); free(canonical);
            if (module && strcmp(module->path,chosen->path)) { package_error(file,token,ERR_MODULE_DEPENDENCY,"Vendored module identity disagrees with require '%s'",chosen->path); return NULL; }
        }
        if (!module) return NULL;
    }
    const char *relative=path+strlen(module->path); if (*relative == '/') relative++;
    char *directory=joined_path(module->root,relative);
    char *canonical=directory && path_is_directory(directory) ? canonical_existing_path(directory) : NULL;
    free(directory);
    if (!canonical) { package_error(file,token,ERR_PACKAGE_NOT_FOUND,"Package '%s' was not found",path); return NULL; }
    if (!path_prefix(canonical,module->root)) { package_error(file,token,ERR_COMP_IMPORT_OUTSIDE_ROOT,"Package '%s' escapes module root '%s'",path,module->root); free(canonical); return NULL; }
    char *probe=joined_path(canonical,".package.dmm");
    char *nearest=probe ? find_module_root(probe) : NULL; free(probe);
    if (!nearest || strcmp(nearest,module->root)) {
        package_error(file,token,ERR_COMP_IMPORT_OUTSIDE_ROOT,"Package '%s' belongs to a different module root",path); free(nearest); free(canonical); return NULL;
    }
    free(nearest);
    /* Every internal segment restricts access to its enclosing canonical subtree. */
    const char *part=path;
    while ((part=strstr(part,"/internal")) != NULL) {
        const char *end=part+9;
        if (*end == '/' || !*end) {
            size_t parent=(size_t)(part-path);
            if (strncmp(file->package->path,path,parent) ||
                (file->package->path[parent] != '\0' && file->package->path[parent] != '/') ||
                strcmp(file->module->path,module->path)) {
                package_error(file,token,ERR_PACKAGE_INTERNAL,"Package '%s' cannot import internal package '%s'",file->package->path,path); free(canonical); return NULL;
            }
        }
        part=end;
    }
    DmmPackage *package=load_package(loader,module,path,canonical); free(canonical);
    if (package && !strcmp(package->name,"main")) { package_error(file,token,ERR_PACKAGE_NAME,"Executable package '%s' cannot be imported",path); return NULL; }
    return package;
}

static int load_package_imports(PackageLoader *loader,DmmPackage *package) {
    for (size_t f=0; f<package->file_count; f++) {
        AstProgram *file=package->files[f];
        for (AstDeclarationNode *d=file->root; d; d=d->next) if (d->kind == AST_DECL_IMPORT)
            for (AstImportPath *entry=d->as.import_decl.paths; entry; entry=entry->next) {
                const char *quoted=ast_program_lexeme(file,entry->path_token); size_t n=strlen(quoted);
                char path[1100]; if (!n || n >= sizeof(path)) return 0;
                memcpy(path,quoted,n+1);
                DmmPackage *target=resolve_package(loader,file,path,entry->path_token);
                if (!target || !target->file_count) return 0;
                entry->resolved_program=target->files[0];
                if (!entry->alias) entry->alias=target->name;
                if (!strcmp(entry->alias,"_")) {
                    package_error(file,entry->path_token,ERR_PACKAGE_ALIAS,"Invalid import alias '%s'",entry->alias); return 0;
                }
                for (AstDeclarationNode *local=file->root; local; local=local->next)
                    if (local->kind != AST_DECL_IMPORT && local->name_token != AST_TOKEN_NONE &&
                        !strcmp(ast_program_lexeme(file,local->name_token),entry->alias)) {
                        package_error(file,entry->path_token,ERR_PACKAGE_ALIAS,"Import alias '%s' conflicts with a declaration",entry->alias); return 0;
                    }
                for (AstDeclarationNode *previous=file->root; previous; previous=previous->next) if (previous->kind == AST_DECL_IMPORT)
                    for (AstImportPath *p=previous->as.import_decl.paths; p && p != entry; p=p->next)
                        if (p->resolved_program && p->alias && !strcmp(p->alias,entry->alias)) {
                            package_error(file,entry->path_token,ERR_PACKAGE_ALIAS,"Duplicate import alias '%s'",entry->alias); return 0;
                        }
            }
    }
    return 1;
}

static DmmPackage *load_package(PackageLoader *loader,DmmModule *module,const char *path,const char *directory) {
    for (DmmPackage *p=loader->root->module->packages; p; p=p->next) if (!strcmp(p->path,path)) {
        if (strcmp(p->directory,directory)) { package_error(loader->root,AST_TOKEN_NONE,ERR_MODULE_DEPENDENCY,"Conflicting physical roots for package '%s'",path); return NULL; }
        if (p->loading) {
            char cycle[4096]={0}; size_t used=0,start=0;
            while (start < loader->depth && loader->stack[start] != p) start++;
            for (size_t i=start;i<loader->depth;i++) {
                int count=snprintf(cycle+used,sizeof(cycle)-used,"%s -> ",loader->stack[i]->path);
                if (count < 0 || (size_t)count >= sizeof(cycle)-used) break;
                used+=(size_t)count;
            }
            package_error(loader->root,AST_TOKEN_NONE,ERR_PACKAGE_CYCLE,"Package import cycle: %s%s",cycle,p->path); return NULL;
        }
        return p;
    }
    if (loader->depth == 128) { package_error(loader->root,AST_TOKEN_NONE,ERR_SEM_COMPLEXITY_LIMIT,"Package graph exceeds 128 import levels"); return NULL; }
    DmmPackage *package=ast_program_alloc(loader->root,sizeof(*package)); if (!package) return NULL;
    package->module=module; package->path=string_interner_intern(loader->root->strings,path);
    package->directory=string_interner_intern(loader->root->strings,directory);
    package->next=loader->root->module->packages; loader->root->module->packages=package;
    DIR *dir=opendir(directory); if (!dir) return NULL;
    char **paths=NULL; size_t count=0; struct dirent *entry; int discovered=1;
    while ((entry=readdir(dir)) != NULL) {
        size_t n=strlen(entry->d_name);
        if (n <= 4 || strcmp(entry->d_name+n-4,".dmm")) continue;
        char *file_path=joined_path(directory,entry->d_name);
        if (!file_path || count == 1024) { free(file_path); discovered=0; break; }
        if (path_is_directory(file_path)) { free(file_path); continue; }
        char **grown=realloc(paths,(count+1)*sizeof(*grown));
        if (!grown) { free(file_path); discovered=0; break; } paths=grown; paths[count++]=file_path;
    }
    closedir(dir); if (count > 1) qsort(paths,count,sizeof(*paths),string_order);
    if (!discovered) package_error(loader->root,AST_TOKEN_NONE,ERR_SEM_COMPLEXITY_LIMIT,"Package discovery exceeded available resources or 1024 files");
    int ok=count != 0 && discovered;
    for (size_t i=0;i<count;i++) {
        char *canonical=canonical_existing_path(paths[i]);
        AstProgram *file=canonical && !strcmp(canonical,loader->root->source_path) ? loader->root : NULL;
        if (!canonical || !path_prefix(canonical,directory)) { package_error(loader->root,AST_TOKEN_NONE,ERR_COMP_IMPORT_OUTSIDE_ROOT,"Source '%s' escapes package directory '%s'",canonical ? canonical : paths[i],directory); ok=0; }
        else if (!file) {
            if (!record_loaded_source(loader->root,canonical)) ok=0;
            file=parse_single_file(canonical,loader->options,loader->root->strings);
            if (file && !append_owned_import(loader->root,file)) { ast_program_free(file); file=NULL; }
        }
        if (!file || !attach_package_file(loader,package,file)) ok=0;
        free(canonical); free(paths[i]);
    }
    free(paths);
    if (!ok) { if (!count) package_error(loader->root,AST_TOKEN_NONE,ERR_PACKAGE_NOT_FOUND,"Package '%s' has no DMM source files",path); return NULL; }
    package->loading=1; loader->stack[loader->depth++]=package;
    ok=load_package_imports(loader,package);
    loader->depth--; package->loading=0; return ok ? package : NULL;
}

static int load_module_packages(AstProgram *root,const FrontendOptions *options) {
    if (!root->structured_ast_complete) return 1;
    PackageLoader loader={.root=root,.options=options};
    char *directory=find_module_root(root->source_path);
    if (!directory) { package_error(root,AST_TOKEN_NONE,ERR_MODULE_MANIFEST,"No dmm.manifest found above the current package"); return 0; }
    root->module=read_module(&loader,directory); free(directory);
    if (!root->module) return 0;
    char *current=source_directory(root->source_path);
    if (!current || !path_prefix(current,root->module->root)) { free(current); return 0; }
    const char *relative=current+strlen(root->module->root); if (*relative == '/') relative++;
    char *path=*relative ? joined_path(root->module->path,relative) : copy_string(root->module->path);
    DmmPackage *package=path ? load_package(&loader,root->module,path,current) : NULL;
    free(path); free(current); return package != NULL;
}
