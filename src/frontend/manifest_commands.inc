/* Manifest commands reuse package loading, without requiring an entry point or
 * type-checking function bodies. The project manifest is changed only after the
 * complete local graph has loaded and all version requirements agree. */
typedef struct {
    const char *path;
    const char *version;
    int used;
    int direct;
} ManifestEntry;

static int manifest_scan_packages(PackageLoader *loader,const char *directory,size_t *visited) {
    if (++*visited > 4096) {
        package_error(loader->root,AST_TOKEN_NONE,ERR_SEM_COMPLEXITY_LIMIT,"Manifest discovery exceeded 4096 directories"); return 0;
    }
    DIR *stream=opendir(directory);
    if (!stream) { package_error(loader->root,AST_TOKEN_NONE,ERR_PACKAGE_NOT_FOUND,"Cannot read package directory '%s'",directory); return 0; }
    int has_source=0,ok=1;
    struct dirent *entry;
    while ((entry=readdir(stream)) != NULL) {
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name,"vendor")) continue;
        char *path=joined_path(directory,entry->d_name);
        if (!path) { ok=0; break; }
        if (path_is_directory(path)) {
            char *manifest=joined_path(path,"dmm.manifest");
            struct stat status;
            int nested=manifest && stat(manifest,&status) == 0;
            free(manifest);
            char *canonical=canonical_existing_path(path);
            /* Do not follow directory aliases, which could create discovery cycles. */
            if (!nested && canonical && !strcmp(path,canonical))
                ok=manifest_scan_packages(loader,canonical,visited);
            free(canonical);
        } else {
            size_t length=strlen(entry->d_name);
            if (length > 4 && !strcmp(entry->d_name+length-4,".dmm")) has_source=1;
        }
        free(path);
        if (!ok) break;
    }
    closedir(stream);
    if (!ok || !has_source) return ok;
    DmmModule *module=loader->root->module;
    const char *relative=directory+strlen(module->root); if (*relative == '/') relative++;
    char *path=*relative ? joined_path(module->path,relative) : copy_string(module->path);
    DmmPackage *package=path ? load_package(loader,module,path,directory) : NULL;
    free(path); return package != NULL;
}

static int manifest_entry_order(const void *a,const void *b) {
    return strcmp(((const ManifestEntry *)a)->path,((const ManifestEntry *)b)->path);
}

static int manifest_write(AstProgram *root,ManifestEntry *entries,size_t count) {
    char *path=joined_path(root->module->root,"dmm.manifest");
    char *temporary=path ? malloc(strlen(path)+5) : NULL;
    if (!path || !temporary) { free(path); free(temporary); return 0; }
    sprintf(temporary,"%s.tmp",path);
#ifdef _WIN32
    int descriptor=_open(temporary,_O_WRONLY|_O_CREAT|_O_EXCL|_O_BINARY,_S_IREAD|_S_IWRITE);
    FILE *output=descriptor >= 0 ? _fdopen(descriptor,"wb") : NULL;
#else
    int descriptor=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);
    FILE *output=descriptor >= 0 ? fdopen(descriptor,"wb") : NULL;
#endif
    if (!output) {
        if (descriptor >= 0) {
#ifdef _WIN32
            (void)_close(descriptor);
#else
            (void)close(descriptor);
#endif
            (void)remove(temporary);
        }
        package_error(root,AST_TOKEN_NONE,ERR_MODULE_MANIFEST,"Cannot create manifest temporary file '%s' (existing files are preserved)",temporary);
        free(path); free(temporary); return 0;
    }
    int ok=fprintf(output,"module %s\n\ndmm %s\n",root->module->path,root->module->version) >= 0;
    size_t retained=0;
    for (size_t i=0;i<count;i++)
        if (entries[i].used) retained++;
    if (retained && fputs("\nrequire (\n",output) == EOF) ok=0;
    for (size_t i=0;i<count;i++) {
        const ManifestEntry *entry=&entries[i];
        if (!entry->used) continue;
        if (fprintf(output,"    %s %s%s\n",entry->path,entry->version,entry->direct ? "" : " // indirect") < 0) ok=0;
    }
    if (retained && fputs(")\n",output) == EOF) ok=0;
    if (fclose(output)) ok=0;
    if (ok) {
#ifdef _WIN32
        ok=MoveFileExA(temporary,path,MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok=rename(temporary,path) == 0;
#endif
    }
    if (!ok) {
        package_error(root,AST_TOKEN_NONE,ERR_MODULE_MANIFEST,"Cannot replace manifest '%s'; original manifest preserved",path);
        (void)remove(temporary);
    }
    free(path); free(temporary); return ok;
}

int frontend_sync_manifest(const char *package_directory) {
    char *current=canonical_existing_path(package_directory);
    char *probe=current ? joined_path(current,".manifest-source.dmm") : NULL;
    char *directory=probe ? find_module_root(probe) : NULL;
    free(current); free(probe);
    if (!directory) {
        error_report(global_error_handler,SEVERITY_ERROR,0,0,ERROR_CATEGORY_SEMANTIC,
            ERR_MODULE_MANIFEST,package_directory,"No dmm.manifest found above the selected directory"); return 0;
    }
    char *source_path=joined_path(directory,".manifest-source.dmm");
    const char *source="package main;";
    FrontendOptions options={0};
    AstProgram *root=source_path ? frontend_parse_source(source,strlen(source),source_path,&options) : NULL;
    free(source_path);
    if (!root) { free(directory); return 0; }
    PackageLoader loader={.root=root,.options=&options};
    root->module=read_module(&loader,directory);
    size_t visited=0;
    int ok=root->module && manifest_scan_packages(&loader,directory,&visited);
    free(directory);
    ManifestEntry entries[1024]; size_t count=0;
    if (ok) {
        for (DmmDependency *dependency=root->module->dependencies;dependency;dependency=dependency->next) {
            if (count == 1024) { ok=0; break; }
            entries[count++]=(ManifestEntry){.path=dependency->path,.version=dependency->version,.direct=!dependency->is_indirect};
        }
    }
    /* Every loaded external module must have one exact local requirement version.
     * Conflicting versions need explicit user resolution, not automatic selection. */
    for (DmmPackage *package=ok ? root->module->packages : NULL;package;package=package->next) {
        if (!strcmp(package->module->path,root->module->path) || !strcmp(package->module->path,"stdlib")) continue;
        size_t index=0;
        while (index<count && strcmp(entries[index].path,package->module->path)) index++;
        if (index == count) {
            if (count == 1024) { ok=0; break; }
            entries[count++]=(ManifestEntry){.path=package->module->path};
        }
        ManifestEntry *target=&entries[index]; target->used=1;
        for (DmmPackage *provider=root->module->packages;provider;provider=provider->next)
            for (DmmDependency *dependency=provider->module->dependencies;dependency;dependency=dependency->next)
                if (!strcmp(dependency->path,target->path)) {
                    if (target->version && strcmp(target->version,dependency->version)) {
                        package_error(root,AST_TOKEN_NONE,ERR_MODULE_DEPENDENCY,"Conflicting require versions for '%s': %s and %s",target->path,target->version,dependency->version); ok=0;
                    }
                    target->version=dependency->version;
                }
        if (!target->version) {
            package_error(root,AST_TOKEN_NONE,ERR_MODULE_DEPENDENCY,"No require version is available for '%s'",target->path); ok=0;
        }
        target->direct=0;
        for (DmmPackage *own=root->module->packages;own;own=own->next) {
            if (strcmp(own->module->path,root->module->path)) continue;
            for (size_t f=0;f<own->file_count;f++)
                for (AstDeclarationNode *d=own->files[f]->root;d;d=d->next)
                    if (d->kind == AST_DECL_IMPORT)
                        for (AstImportPath *import=d->as.import_decl.paths;import;import=import->next)
                            if (import->resolved_program && !strcmp(import->resolved_program->module->path,target->path)) target->direct=1;
        }
    }
    if (!ok && !error_handler_get_error_count(global_error_handler))
        package_error(root,AST_TOKEN_NONE,ERR_MODULE_MANIFEST,"Cannot analyze manifest dependency graph (resource limit)");
    if (ok && !error_handler_get_error_count(global_error_handler)) {
        if (count>1) qsort(entries,count,sizeof(*entries),manifest_entry_order);
        ok=manifest_write(root,entries,count);
    } else ok=0;
    ast_program_free(root); return ok;
}
