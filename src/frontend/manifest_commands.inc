/* Manifest commands reuse package loading, without requiring an entry point or
 * type-checking function bodies. The project manifest is changed only after the
 * complete local graph has loaded and all version requirements agree. */
typedef struct {
    const char *path;
    const char *version;
    int used;
    int direct;
} ManifestEntry;

static int manifest_scan_packages(PackageLoader *loader, const char *directory, size_t *visited) {
    if (++*visited > 4096) {
        package_error(loader->root, AST_TOKEN_NONE, ERR_SEM_COMPLEXITY_LIMIT,
                      "Manifest discovery exceeded 4096 directories");
        return 0;
    }
    if (loader->options && loader->options->workspace_directory &&
        !loader->options->workspace_directory(loader->options->workspace_directory_context, directory, 0)) {
        package_error(loader->root, AST_TOKEN_NONE, ERR_SEM_COMPLEXITY_LIMIT,
                      "Cannot capture workspace directory inventory for '%s'", directory);
        return 0;
    }
    DIR *stream = opendir(directory);
    if (!stream) {
        package_error(loader->root, AST_TOKEN_NONE, ERR_PACKAGE_NOT_FOUND, "Cannot read package directory '%s'",
                      directory);
        return 0;
    }
    int has_source = 0, ok = 1;
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        if (entry->d_name[0] == '.' || !strcmp(entry->d_name, "vendor")) continue;
        char *path = joined_path(directory, entry->d_name);
        if (!path) {
            ok = 0;
            break;
        }
        if (path_is_directory(path)) {
            char *manifest = joined_path(path, "dmm.manifest");
            struct stat status;
            int nested = manifest && (error_handler_overlay(manifest) || stat(manifest, &status) == 0);
            if (nested && loader->options && loader->options->workspace_directory &&
                !loader->options->workspace_directory(loader->options->workspace_directory_context, path, 1))
                ok = 0;
            free(manifest);
            char *canonical = canonical_existing_path(path);
            /* Do not follow directory aliases, which could create discovery cycles. */
            if (!nested && canonical && !strcmp(path, canonical))
                ok = manifest_scan_packages(loader, canonical, visited);
            free(canonical);
        } else {
            size_t length = strlen(entry->d_name);
            if (length > 4 && !strcmp(entry->d_name + length - 4, ".dmm")) has_source = 1;
        }
        free(path);
        if (!ok) break;
    }
    closedir(stream);
    if (!has_source && global_error_handler) {
        size_t prefix = strlen(directory);
        for (size_t i = 0; i < global_error_handler->overlay_count; i++) {
            const char *path = global_error_handler->overlays[i].path;
            if (strncmp(path, directory, prefix) || path[prefix] != '/') continue;
            const char *leaf = path + prefix + 1;
            size_t length = strlen(leaf);
            if (!strchr(leaf, '/') && length > 4 && !strcmp(leaf + length - 4, ".dmm")) has_source = 1;
        }
    }
    if (!ok || !has_source) return ok;
    DmmModule *module = loader->root->module;
    const char *relative = directory + strlen(module->root);
    if (*relative == '/') relative++;
    char *path = *relative ? joined_path(module->path, relative) : copy_string(module->path);
    DmmPackage *package = path ? load_package(loader, module, path, directory) : NULL;
    free(path);
    return package != NULL;
}

int frontend_discover_workspace(AstProgram *program, const FrontendOptions *options) {
    if (!program || !program->module) return 0;
    PackageLoader loader = {.root = program, .options = options};
    size_t visited = 0;
    return manifest_scan_packages(&loader, program->module->root, &visited);
}

static int manifest_entry_order(const void *a, const void *b) {
    return strcmp(((const ManifestEntry *) a)->path, ((const ManifestEntry *) b)->path);
}

/* Documentation survives dependency synchronization, including unrelated blocks. */
static int manifest_copy_comments(FILE *output, const char *path) {
    FILE *input = error_handler_open_source(path);
    if (!input) return 0;
    int c, ok = 1;
    while (ok && (c = fgetc(input)) != EOF) {
        if (c != '/') continue;
        int next = fgetc(input);
        if (next == '/') {
            long comment = ftell(input) - 2;
            char body[128] = {0};
            size_t length = 0;
            while ((c = fgetc(input)) != EOF && c != '\n') {
                if (length < sizeof(body) - 1) body[length] = (char)c;
                length++;
            }
            long finish = ftell(input);
            int generated = 0;
            if (length < sizeof(body)) {
                size_t start = 0, end = length;
                while (start < end && (body[start] == ' ' || body[start] == '\t')) start++;
                while (end > start && (body[end - 1] == ' ' || body[end - 1] == '\t' || body[end - 1] == '\r')) end--;
                if (end - start == 8 && !strncmp(body + start, "indirect", 8)) {
                    /* Only omit the generated requirement annotation, not user documentation. */
                    long begin = comment;
                    while (begin > 0 && comment - begin < 4096) {
                        if (fseek(input, begin - 1, SEEK_SET)) {ok = 0;break;}
                        if (fgetc(input) == '\n') break;
                        begin--;
                    }
                    if (comment - begin < 4096 && !fseek(input, begin, SEEK_SET)) {
                        char prefix[4096] = {0}, dependency[2048] = {0}, version[64] = {0};
                        if (fread(prefix, 1, (size_t)(comment - begin), input) != (size_t)(comment - begin)) ok = 0;
                        generated = sscanf(prefix, " %2047s %63s", dependency, version) == 2 &&
                            version[0] == 'v' && version[1] >= '0' && version[1] <= '9' && strchr(version, '.');
                        if (!generated && !strcmp(dependency, "require")) {
                            char directive[16];
                            generated = sscanf(prefix, " %15s %2047s %63s", directive, dependency, version) == 3 &&
                                version[0] == 'v' && version[1] >= '0' && version[1] <= '9' && strchr(version, '.');
                        }
                    }
                }
            }
            if (!generated && ok) {
                if (fseek(input, comment, SEEK_SET)) ok = 0;
                else {
                    while ((c = fgetc(input)) != EOF && c != '\n') if (fputc(c, output) == EOF) ok = 0;
                    if (fputs("\n\n", output) == EOF) ok = 0;
                }
            }
            if (finish < 0 || fseek(input, finish, SEEK_SET)) ok = 0;
        } else if (next == '*') {
            if (fputs("/*", output) == EOF) {
                ok = 0;
                break;
            }
            int previous = 0, closed = 0;
            while ((c = fgetc(input)) != EOF) {
                if (fputc(c, output) == EOF) {
                    ok = 0;
                    break;
                }
                if (previous == '*' && c == '/') {
                    closed = 1;
                    break;
                }
                previous = c;
            }
            if (!closed || fputs("\n\n", output) == EOF) ok = 0;
        } else if (next != EOF) ungetc(next, input);
    }
    if (ferror(input)) ok = 0;
    fclose(input);
    return ok;
}

static int manifest_render(FILE *output, AstProgram *root, ManifestEntry *entries, size_t count) {
    char *path = joined_path(root->module->root, "dmm.manifest");
    int ok = path && manifest_copy_comments(output, path);
    free(path);
    if (fprintf(output, "module %s\n\ndmm %s\n", root->module->path, root->module->edition) < 0) ok = 0;
    size_t feature_count = 0;
    for (DmmFeature *feature = root->module->features; feature; feature = feature->next) feature_count++;
    const char **features = feature_count ? malloc(feature_count * sizeof(*features)) : NULL;
    if (feature_count && !features) ok = 0;
    size_t feature_index = 0;
    for (DmmFeature *feature = root->module->features; features && feature; feature = feature->next)
        features[feature_index++] = feature->name;
    if (features && feature_count > 1) qsort(features, feature_count, sizeof(*features), string_order);
    if (features && fputs("\nfeatures = [\n", output) == EOF) ok = 0;
    for (size_t i = 0; features && i < feature_count; i++)
        if (fprintf(output, "    \"%s\",\n", features[i]) < 0) ok = 0;
    if (features && fputs("]\n", output) == EOF) ok = 0;
    free(features);
    size_t retained = 0;
    for (size_t i = 0; i < count; i++)
        if (entries[i].used) retained++;
    if (retained && fputs("\nrequire (\n", output) == EOF) ok = 0;
    for (size_t i = 0; i < count; i++) {
        const ManifestEntry *entry = &entries[i];
        if (!entry->used) continue;
        if (fprintf(output, "    %s %s%s\n", entry->path, entry->version, entry->direct ? "" : " // indirect") < 0)
            ok = 0;
    }
    if (retained && fputs(")\n", output) == EOF) ok = 0;
    return ok && !ferror(output);
}

static int manifest_write(AstProgram *root, ManifestEntry *entries, size_t count) {
    char *path = joined_path(root->module->root, "dmm.manifest");
    char *temporary = path ? malloc(strlen(path) + 5) : NULL;
    if (!path || !temporary) {
        free(path);
        free(temporary);
        return 0;
    }
    sprintf(temporary, "%s.tmp", path);
#ifdef _WIN32
    int descriptor = _open(temporary, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
    FILE *output = descriptor >= 0 ? _fdopen(descriptor, "wb") : NULL;
#else
    int descriptor = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE *output = descriptor >= 0 ? fdopen(descriptor, "wb") : NULL;
#endif
    if (!output) {
        if (descriptor >= 0) {
#ifdef _WIN32
            (void) _close(descriptor);
#else
            (void) close(descriptor);
#endif
            (void) remove(temporary);
        }
        package_error(root, AST_TOKEN_NONE, ERR_MODULE_MANIFEST,
                      "Cannot create manifest temporary file '%s' (existing files are preserved)", temporary);
        free(path);
        free(temporary);
        return 0;
    }
    int ok = manifest_render(output, root, entries, count);
    if (fclose(output)) ok = 0;
    if (ok) {
#ifdef _WIN32
        ok = MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = rename(temporary, path) == 0;
#endif
    }
    if (!ok) {
        package_error(root, AST_TOKEN_NONE, ERR_MODULE_MANIFEST,
                      "Cannot replace manifest '%s'; original manifest preserved", path);
        (void) remove(temporary);
    }
    free(path);
    free(temporary);
    return ok;
}

static int manifest_sync(const char *package_directory, FILE *preview, char **manifest_path) {
    char *current = canonical_existing_path(package_directory);
    char *probe = current ? joined_path(current, ".manifest-source.dmm") : NULL;
    char *directory = probe ? find_module_root(probe) : NULL;
    free(current);
    free(probe);
    if (!directory) {
        error_report(global_error_handler, SEVERITY_ERROR, 0, 0, ERROR_CATEGORY_SEMANTIC,
                     ERR_MODULE_MANIFEST, package_directory, "No dmm.manifest found above the selected directory");
        return 0;
    }
    char *source_path = joined_path(directory, ".manifest-source.dmm");
    const char *source = "package main;";
    FrontendOptions options = {.include_all_targets = 1};
    AstProgram *root = source_path ? frontend_parse_source(source, strlen(source), source_path, &options) : NULL;
    free(source_path);
    if (!root) {
        free(directory);
        return 0;
    }
    PackageLoader loader = {.root = root, .options = &options};
    root->module = read_module(&loader, directory);
    size_t visited = 0;
    int ok = root->module && manifest_scan_packages(&loader, directory, &visited);
    free(directory);
    ManifestEntry entries[1024];
    size_t count = 0;
    if (ok) {
        for (DmmDependency *dependency = root->module->dependencies; dependency; dependency = dependency->next) {
            if (count == 1024) {
                ok = 0;
                break;
            }
            entries[count++] = (ManifestEntry){
                .path = dependency->path, .version = dependency->version, .direct = !dependency->is_indirect
            };
        }
    }
    /* Every loaded external module must have one exact local requirement version.
     * Conflicting versions need explicit user resolution, not automatic selection. */
    for (DmmPackage *package = ok ? root->module->packages : NULL; package; package = package->next) {
        if (!strcmp(package->module->path, root->module->path) || !strcmp(package->module->path, "stdlib")) continue;
        size_t index = 0;
        while (index < count && strcmp(entries[index].path, package->module->path)) index++;
        if (index == count) {
            if (count == 1024) {
                ok = 0;
                break;
            }
            entries[count++] = (ManifestEntry){.path = package->module->path};
        }
        ManifestEntry *target = &entries[index];
        target->used = 1;
        for (DmmPackage *provider = root->module->packages; provider; provider = provider->next)
            for (DmmDependency *dependency = provider->module->dependencies; dependency; dependency = dependency->next)
                if (!strcmp(dependency->path, target->path)) {
                    if (target->version && strcmp(target->version, dependency->version)) {
                        package_error(root, AST_TOKEN_NONE, ERR_MODULE_DEPENDENCY,
                                      "Conflicting require versions for '%s': %s and %s", target->path, target->version,
                                      dependency->version);
                        ok = 0;
                    }
                    target->version = dependency->version;
                }
        if (!target->version) {
            package_error(root, AST_TOKEN_NONE, ERR_MODULE_DEPENDENCY, "No require version is available for '%s'",
                          target->path);
            ok = 0;
        }
        target->direct = 0;
        for (DmmPackage *own = root->module->packages; own; own = own->next) {
            if (strcmp(own->module->path, root->module->path)) continue;
            for (size_t f = 0; f < own->file_count; f++)
                for (AstDeclarationNode *d = own->files[f]->root; d; d = d->next)
                    if (d->kind == AST_DECL_IMPORT)
                        for (AstImportPath *import = d->as.import_decl.paths; import; import = import->next)
                            if (import->resolved_program && !strcmp(import->resolved_program->module->path,
                                                                    target->path))
                                target->direct = 1;
        }
    }
    if (!ok && !error_handler_get_error_count(global_error_handler))
        package_error(root, AST_TOKEN_NONE, ERR_MODULE_MANIFEST,
                      "Cannot analyze manifest dependency graph (resource limit)");
    if (ok && !error_handler_get_error_count(global_error_handler)) {
        if (count > 1) qsort(entries, count, sizeof(*entries), manifest_entry_order);
        ok = preview ? manifest_render(preview, root, entries, count) : manifest_write(root, entries, count);
        if (ok && manifest_path) {
            *manifest_path = joined_path(root->module->root, "dmm.manifest");
            if (!*manifest_path) ok = 0;
        }
    } else ok = 0;
    ast_program_free(root);
    return ok;
}

int frontend_sync_manifest(const char *package_directory) {
    return manifest_sync(package_directory, NULL, NULL);
}

int frontend_preview_manifest(const char *package_directory, FILE *output, char **manifest_path) {
    if (!output || !manifest_path) return 0;
    *manifest_path = NULL;
    return manifest_sync(package_directory, output, manifest_path);
}
