/*
  Copyright 2007-2011 David Robillard <http://drobilla.net>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

#define _XOPEN_SOURCE 500

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <dirent.h>
#ifdef HAVE_WORDEXP
#include <wordexp.h>
#endif

#include "lilv_internal.h"

LILV_API
LilvWorld*
lilv_world_new(void)
{
	LilvWorld* world = malloc(sizeof(struct LilvWorldImpl));

	world->world = sord_world_new();
	if (!world->world)
		goto fail;

	world->model = sord_new(world->world, SORD_SPO|SORD_OPS, true);
	if (!world->model)
		goto fail;

	world->specs          = NULL;
	world->plugin_classes = lilv_plugin_classes_new();
	world->plugins        = lilv_plugins_new();

#define NS_DYNMAN "http://lv2plug.in/ns/ext/dynmanifest#"
#define NS_DC     "http://dublincore.org/documents/dcmi-namespace/"

#define NEW_URI(uri)     sord_new_uri(world->world, (const uint8_t*)uri)
#define NEW_URI_VAL(uri) lilv_new_uri(world, (const char*)(uri));

	world->dc_replaces_node         = NEW_URI(NS_DC        "replaces");
	world->dyn_manifest_node        = NEW_URI(NS_DYNMAN    "DynManifest");
	world->lv2_binary_node          = NEW_URI(LILV_NS_LV2  "binary");
	world->lv2_default_node         = NEW_URI(LILV_NS_LV2  "default");
	world->lv2_index_node           = NEW_URI(LILV_NS_LV2  "index");
	world->lv2_maximum_node         = NEW_URI(LILV_NS_LV2  "maximum");
	world->lv2_minimum_node         = NEW_URI(LILV_NS_LV2  "minimum");
	world->lv2_plugin_node          = NEW_URI(LILV_NS_LV2  "Plugin");
	world->lv2_port_node            = NEW_URI(LILV_NS_LV2  "port");
	world->lv2_portproperty_node    = NEW_URI(LILV_NS_LV2  "portProperty");
	world->lv2_reportslatency_node  = NEW_URI(LILV_NS_LV2  "reportsLatency");
	world->lv2_specification_node   = NEW_URI(LILV_NS_LV2  "Specification");
	world->lv2_symbol_node          = NEW_URI(LILV_NS_LV2  "symbol");
	world->rdf_a_node               = NEW_URI(LILV_NS_RDF  "type");
	world->rdf_value_node           = NEW_URI(LILV_NS_RDF  "value");
	world->rdfs_class_node          = NEW_URI(LILV_NS_RDFS "Class");
	world->rdfs_label_node          = NEW_URI(LILV_NS_RDFS "label");
	world->rdfs_seealso_node        = NEW_URI(LILV_NS_RDFS "seeAlso");
	world->rdfs_subclassof_node     = NEW_URI(LILV_NS_RDFS "subClassOf");
	world->xsd_boolean_node         = NEW_URI(LILV_NS_XSD  "boolean");
	world->xsd_decimal_node         = NEW_URI(LILV_NS_XSD  "decimal");
	world->xsd_double_node          = NEW_URI(LILV_NS_XSD  "double");
	world->xsd_integer_node         = NEW_URI(LILV_NS_XSD  "integer");

	world->doap_name_val           = NEW_URI_VAL(LILV_NS_DOAP "name");
	world->lv2_name_val            = NEW_URI_VAL(LILV_NS_LV2  "name");
	world->lv2_optionalFeature_val = NEW_URI_VAL(LILV_NS_LV2  "optionalFeature");
	world->lv2_requiredFeature_val = NEW_URI_VAL(LILV_NS_LV2  "requiredFeature");

	world->lv2_plugin_class = lilv_plugin_class_new(
		world, NULL, world->lv2_plugin_node, "Plugin");
	assert(world->lv2_plugin_class);

	world->n_read_files        = 0;
	world->opt.filter_language = true;
	world->opt.dyn_manifest    = true;

	return world;

fail:
	/* keep on rockin' in the */ free(world);
	return NULL;
}

LILV_API
void
lilv_world_free(LilvWorld* world)
{
	if (!world) {
		return;
	}

	lilv_plugin_class_free(world->lv2_plugin_class);
	world->lv2_plugin_class = NULL;

	sord_node_free(world->world, world->dc_replaces_node);
	sord_node_free(world->world, world->dyn_manifest_node);
	sord_node_free(world->world, world->lv2_binary_node);
	sord_node_free(world->world, world->lv2_default_node);
	sord_node_free(world->world, world->lv2_index_node);
	sord_node_free(world->world, world->lv2_maximum_node);
	sord_node_free(world->world, world->lv2_minimum_node);
	sord_node_free(world->world, world->lv2_plugin_node);
	sord_node_free(world->world, world->lv2_port_node);
	sord_node_free(world->world, world->lv2_portproperty_node);
	sord_node_free(world->world, world->lv2_reportslatency_node);
	sord_node_free(world->world, world->lv2_specification_node);
	sord_node_free(world->world, world->lv2_symbol_node);
	sord_node_free(world->world, world->rdf_a_node);
	sord_node_free(world->world, world->rdf_value_node);
	sord_node_free(world->world, world->rdfs_class_node);
	sord_node_free(world->world, world->rdfs_label_node);
	sord_node_free(world->world, world->rdfs_seealso_node);
	sord_node_free(world->world, world->rdfs_subclassof_node);
	sord_node_free(world->world, world->xsd_boolean_node);
	sord_node_free(world->world, world->xsd_decimal_node);
	sord_node_free(world->world, world->xsd_double_node);
	sord_node_free(world->world, world->xsd_integer_node);
	lilv_node_free(world->doap_name_val);
	lilv_node_free(world->lv2_name_val);
	lilv_node_free(world->lv2_optionalFeature_val);
	lilv_node_free(world->lv2_requiredFeature_val);

	for (LilvSpec* spec = world->specs; spec;) {
		LilvSpec* next = spec->next;
		sord_node_free(world->world, spec->spec);
		sord_node_free(world->world, spec->bundle);
		lilv_nodes_free(spec->data_uris);
		free(spec);
		spec = next;
	}
	world->specs = NULL;

	LILV_FOREACH(plugins, i, world->plugins) {
		const LilvPlugin* p = lilv_plugins_get(world->plugins, i);
		lilv_plugin_free((LilvPlugin*)p);
	}
	zix_tree_free(world->plugins);
	world->plugins = NULL;

	zix_tree_free(world->plugin_classes);
	world->plugin_classes = NULL;

	sord_free(world->model);
	world->model = NULL;

	sord_world_free(world->world);
	world->world = NULL;

	free(world);
}

LILV_API
void
lilv_world_set_option(LilvWorld*       world,
                      const char*      option,
                      const LilvNode* value)
{
	if (!strcmp(option, LILV_OPTION_DYN_MANIFEST)) {
		if (lilv_node_is_bool(value)) {
			world->opt.dyn_manifest = lilv_node_as_bool(value);
			return;
		}
	} else if (!strcmp(option, LILV_OPTION_FILTER_LANG)) {
		if (lilv_node_is_bool(value)) {
			world->opt.filter_language = lilv_node_as_bool(value);
			return;
		}
	}
	LILV_WARNF("Unrecognized or invalid option `%s'\n", option);
}

static SordIter*
lilv_world_find_statements(const LilvWorld* world,
                           SordModel*       model,
                           const SordNode*  subject,
                           const SordNode*  predicate,
                           const SordNode*  object,
                           const SordNode*  graph)
{
	SordQuad pat = { subject, predicate, object, graph };
	return sord_find(model, pat);
}

LILV_API
LilvNodes*
lilv_world_find_nodes(LilvWorld*      world,
                      const LilvNode* subject,
                      const LilvNode* predicate,
                      const LilvNode* object)
{
	if (!lilv_node_is_uri(subject) && !lilv_node_is_blank(subject)) {
		LILV_ERRORF("Subject `%s' is not a resource\n", subject->str_val);
		return NULL;
	}
	if (!lilv_node_is_uri(predicate)) {
		LILV_ERRORF("Predicate `%s' is not a URI\n", predicate->str_val);
		return NULL;
	}

	SordNode* subject_node = (lilv_node_is_uri(subject))
		? sord_node_copy(subject->val.uri_val)
		: sord_new_blank(world->world,
		                 (const uint8_t*)lilv_node_as_blank(subject));

	LilvNodes* ret = lilv_world_query_values_internal(world,
	                                                  subject_node,
	                                                  predicate->val.uri_val,
	                                                  NULL);

	sord_node_free(world->world, subject_node);
	return ret;
}

SordIter*
lilv_world_query_internal(LilvWorld*      world,
                          const SordNode* subject,
                          const SordNode* predicate,
                          const SordNode* object)
{
	return lilv_world_find_statements(world, world->model,
	                                  subject, predicate, object, NULL);
}

LilvNodes*
lilv_world_query_values_internal(LilvWorld*      world,
                                 const SordNode* subject,
                                 const SordNode* predicate,
                                 const SordNode* object)
{
	return lilv_nodes_from_stream_objects(
		world,
		lilv_world_query_internal(world, subject, predicate, object));
}

static SerdNode
lilv_new_uri_relative_to_base(const uint8_t* uri_str,
                              const uint8_t* base_uri_str)
{
	SerdURI base_uri;
	if (serd_uri_parse(base_uri_str, &base_uri)) {
		return SERD_NODE_NULL;
	}

	SerdURI ignored;
	return serd_node_new_uri_from_string(uri_str, &base_uri, &ignored);
}

const uint8_t*
lilv_world_blank_node_prefix(LilvWorld* world)
{
	static char str[32];
	snprintf(str, sizeof(str), "%d", world->n_read_files++);
	return (const uint8_t*)str;
}

/** Comparator for sequences (e.g. world->plugins). */
int
lilv_header_compare_by_uri(const void* a, const void* b, void* user_data)
{
	const struct LilvHeader* const header_a = (const struct LilvHeader*)a;
	const struct LilvHeader* const header_b = (const struct LilvHeader*)b;
	return strcmp(lilv_node_as_uri(header_a->uri),
	              lilv_node_as_uri(header_b->uri));
}

/** Get an element of a collection of any object with an LilvHeader by URI. */
struct LilvHeader*
lilv_collection_get_by_uri(const ZixTree*  const_seq,
                           const LilvNode* uri)
{
	ZixTree*          seq = (ZixTree*)const_seq;
	struct LilvHeader key = { NULL, (LilvNode*)uri };

	ZixTreeIter* i  = NULL;
	ZixStatus    st = zix_tree_find(seq, &key, &i);
	if (!st) {
		return (struct LilvHeader*)zix_tree_get(i);
	}

	return NULL;
}

static void
lilv_world_add_spec(LilvWorld*      world,
                    const SordNode* specification_node,
                    const SordNode* bundle_node)
{
	LilvSpec* spec = malloc(sizeof(struct LilvSpecImpl));
	spec->spec      = sord_node_copy(specification_node);
	spec->bundle    = sord_node_copy(bundle_node);
	spec->data_uris = lilv_nodes_new();

	// Add all plugin data files (rdfs:seeAlso)
	SordIter* files = lilv_world_find_statements(
		world, world->model,
		specification_node,
		world->rdfs_seealso_node,
		NULL,
		NULL);
	FOREACH_MATCH(files) {
		const SordNode* file_node = lilv_match_object(files);
		zix_tree_insert(spec->data_uris,
		                lilv_node_new_from_node(world, file_node),
		                NULL);
	}
	lilv_match_end(files);

	// Add specification to world specification list
	spec->next   = world->specs;
	world->specs = spec;
}

static void
lilv_world_add_plugin(LilvWorld*      world,
                      const SordNode* plugin_node,
                      SerdNode*       manifest_uri,
                      const SordNode* dyn_manifest_lib,
                      const SordNode* bundle_node)
{
	LilvNode* plugin_uri = lilv_node_new_from_node(world, plugin_node);

	const LilvPlugin* last = lilv_plugins_get_by_uri(world->plugins, plugin_uri);
	if (last) {
		LILV_ERRORF("Duplicate plugin <%s>\n", lilv_node_as_uri(plugin_uri));
		LILV_ERRORF("... found in %s\n", lilv_node_as_string(
			            lilv_plugin_get_bundle_uri(last)));
		LILV_ERRORF("... and      %s\n", sord_node_get_string(bundle_node));
		lilv_node_free(plugin_uri);
		return;
	}

	// Create LilvPlugin
	LilvNode*   bundle_uri = lilv_node_new_from_node(world, bundle_node);
	LilvPlugin* plugin     = lilv_plugin_new(world, plugin_uri, bundle_uri);

	// Add manifest as plugin data file (as if it were rdfs:seeAlso)
	zix_tree_insert(plugin->data_uris,
	                lilv_new_uri(world, (const char*)manifest_uri->buf),
	                NULL);

	// Set dynamic manifest library URI, if applicable
	if (dyn_manifest_lib) {
		plugin->dynman_uri = lilv_node_new_from_node(world, dyn_manifest_lib);
	}

	// Add all plugin data files (rdfs:seeAlso)
	SordIter* files = lilv_world_find_statements(
		world, world->model,
		plugin_node,
		world->rdfs_seealso_node,
		NULL,
		NULL);
	FOREACH_MATCH(files) {
		const SordNode* file_node = lilv_match_object(files);
		zix_tree_insert(plugin->data_uris,
		                lilv_node_new_from_node(world, file_node),
		                NULL);
	}
	lilv_match_end(files);

	// Add plugin to world plugin sequence
	zix_tree_insert(world->plugins, plugin, NULL);
}

static void
lilv_world_load_dyn_manifest(LilvWorld* world,
                             SordNode*  bundle_node,
                             SerdNode   manifest_uri)
{
#ifdef LILV_DYN_MANIFEST
	if (!world->opt.dyn_manifest) {
		return;
	}

	typedef void* LV2_Dyn_Manifest_Handle;
	LV2_Dyn_Manifest_Handle handle = NULL;

	// ?dman a dynman:DynManifest
	SordIter* dmanifests = lilv_world_find_statements(
		world, world->model,
		NULL,
		world->rdf_a_node,
		world->dyn_manifest_node,
		bundle_node);
	FOREACH_MATCH(dmanifests) {
		const SordNode* dmanifest = lilv_match_subject(dmanifests);

		// ?dman lv2:binary ?binary
		SordIter* binaries  = lilv_world_find_statements(
			world, world->model,
			dmanifest,
			world->lv2_binary_node,
			NULL,
			bundle_node);
		if (lilv_matches_end(binaries)) {
			lilv_match_end(binaries);
			LILV_ERRORF("Dynamic manifest in <%s> has no binaries, ignored\n",
			            sord_node_get_string(bundle_node));
			continue;
		}

		// Get binary path
		const SordNode* binary   = lilv_match_object(binaries);
		const uint8_t*  lib_uri  = sord_node_get_string(binary);
		const char*     lib_path = lilv_uri_to_path((const char*)lib_uri);
		if (!lib_path) {
			LILV_ERROR("No dynamic manifest library path\n");
			lilv_match_end(binaries);
			continue;
		}

		// Open library
		void* lib = dlopen(lib_path, RTLD_LAZY);
		if (!lib) {
			LILV_ERRORF("Failed to open dynamic manifest library `%s'\n", lib_path);
			lilv_match_end(binaries);
			continue;
		}

		// Open dynamic manifest
		typedef int (*OpenFunc)(LV2_Dyn_Manifest_Handle*, const LV2_Feature *const *);
		OpenFunc open_func = (OpenFunc)lilv_dlfunc(lib, "lv2_dyn_manifest_open");
		if (!open_func || open_func(&handle, &dman_features)) {
			LILV_ERRORF("Failed to find `lv2_dyn_manifest_open' in `%s'\n", lib_path);
			lilv_match_end(binaries);
			dlclose(lib);
			continue;
		}

		// Get subjects (the data that would be in manifest.ttl)
		typedef int (*GetSubjectsFunc)(LV2_Dyn_Manifest_Handle, FILE*);
		GetSubjectsFunc get_subjects_func = (GetSubjectsFunc)lilv_dlfunc(
			lib, "lv2_dyn_manifest_get_subjects");
		if (!get_subjects_func) {
			LILV_ERRORF("Failed to find `lv2_dyn_manifest_get_subjects' in `%s'\n",
			            lib_path);
			lilv_match_end(binaries);
			dlclose(lib);
			continue;
		}

		// Generate data file
		FILE* fd = tmpfile();
		get_subjects_func(handle, fd);
		rewind(fd);

		// Parse generated data file
		SerdEnv*    env    = serd_env_new(sord_node_to_serd_node(bundle_node));
		SerdReader* reader = sord_new_reader(
			world->model, env, SERD_TURTLE, bundle_node);
		serd_reader_read_file_handle(reader, fd,
		                             (const uint8_t*)"(dyn-manifest)");
		serd_reader_free(reader);
		serd_env_free(env);

		// Close (and automatically delete) temporary data file
		fclose(fd);

		// ?plugin a lv2:Plugin
		SordIter* plug_results = lilv_world_find_statements(
			world, world->model,
			NULL,
			world->rdf_a_node,
			world->lv2_plugin_node,
			bundle_node);
		FOREACH_MATCH(plug_results) {
			const SordNode* plugin_node = lilv_match_subject(plug_results);
			lilv_world_add_plugin(world, plugin_node,
			                      &manifest_uri, binary, bundle_node);
		}
		lilv_match_end(plug_results);

		lilv_match_end(binaries);
		dlclose(lib);
	}
	lilv_match_end(dmanifests);
#endif  // LILV_DYN_MANIFEST
}

LILV_API
void
lilv_world_load_bundle(LilvWorld* world, LilvNode* bundle_uri)
{
	if (!lilv_node_is_uri(bundle_uri)) {
		LILV_ERRORF("Bundle URI `%s' is not a URI\n", bundle_uri->str_val);
		return;
	}

	SordNode* bundle_node = bundle_uri->val.uri_val;

	SerdNode manifest_uri = lilv_new_uri_relative_to_base(
		(const uint8_t*)"manifest.ttl",
		(const uint8_t*)sord_node_get_string(bundle_node));

	SerdEnv*    env    = serd_env_new(&manifest_uri);
	SerdReader* reader = sord_new_reader(world->model, env, SERD_TURTLE,
	                                     bundle_node);
	serd_reader_add_blank_prefix(reader, lilv_world_blank_node_prefix(world));

	SerdStatus st = serd_reader_read_file(reader, manifest_uri.buf);
	serd_reader_free(reader);
	serd_env_free(env);
	if (st) {
		LILV_ERRORF("Error reading %s\n", manifest_uri.buf);
		return;
	}

	// ?plugin a lv2:Plugin
	SordIter* plug_results = lilv_world_find_statements(
		world, world->model,
		NULL,
		world->rdf_a_node,
		world->lv2_plugin_node,
		bundle_node);
	FOREACH_MATCH(plug_results) {
		const SordNode* plugin_node = lilv_match_subject(plug_results);
		lilv_world_add_plugin(world, plugin_node,
		                      &manifest_uri, NULL, bundle_node);
	}
	lilv_match_end(plug_results);

	lilv_world_load_dyn_manifest(world, bundle_node, manifest_uri);

	// ?specification a lv2:Specification
	SordIter* spec_results = lilv_world_find_statements(
		world, world->model,
		NULL,
		world->rdf_a_node,
		world->lv2_specification_node,
		bundle_node);
	FOREACH_MATCH(spec_results) {
		const SordNode* spec = lilv_match_subject(spec_results);
		lilv_world_add_spec(world, spec, bundle_node);
	}
	lilv_match_end(spec_results);

	serd_node_free(&manifest_uri);
}

/** Expand variables (e.g. POSIX ~ or $FOO, Windows %FOO%) in @a path. */
static char*
expand(const char* path)
{
#ifdef HAVE_WORDEXP
	char*     ret = NULL;
	wordexp_t p;
	wordexp(path, &p, 0);
	if (p.we_wordc == 0) {
		/* Literal directory path (e.g. no variables or ~) */
		ret = lilv_strdup(path);
	} else if (p.we_wordc == 1) {
		/* Directory path expands (e.g. contains ~ or $FOO) */
		ret = lilv_strdup(p.we_wordv[0]);
	} else {
		/* Multiple expansions in a single directory path? */
		LILV_ERRORF("Malformed path `%s' ignored\n", path);
	}
	wordfree(&p);
#elif defined(__WIN32__)
	static const size_t len = 32767;
	char*               ret = malloc(len);
	ExpandEnvironmentStrings(path, ret, len);
#else
	char* ret = lilv_strdup(path);
#endif
	return ret;
}

/** Load all bundles in the directory at @a dir_path. */
static void
lilv_world_load_directory(LilvWorld* world, const char* dir_path)
{
	char* path = expand(dir_path);
	if (!path) {
		LILV_WARNF("Empty path `%s'\n", path);
		return;
	}

	DIR* pdir = opendir(path);
	if (!pdir) {
		free(path);
		return;
	}

#ifdef __WIN32__
	static const char* const file_scheme = "file:///";
#else
	static const char* const file_scheme = "file://";
#endif

	struct dirent  entry;
	struct dirent* result;
	while (!readdir_r(pdir, &entry, &result) && result) {
		if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
			continue;

		char* uri = lilv_strjoin(file_scheme,
		                         path, "/",
		                         entry.d_name, "/",
		                         NULL);

		LilvNode* uri_val = lilv_new_uri(world, uri);
		lilv_world_load_bundle(world, uri_val);
		lilv_node_free(uri_val);

		free(uri);
	}

	free(path);
	closedir(pdir);
}

static bool
is_path_sep(char c)
{
	return c == LILV_PATH_SEP[0];
}

static const char*
first_path_sep(const char* path)
{
	for (const char* p = path; *p != '\0'; ++p) {
		if (is_path_sep(*p)) {
			return p;
		}
	}
	return NULL;
}

/** Load all bundles found in @a lv2_path.
 * @param lv2_path A colon-delimited list of directories.  These directories
 * should contain LV2 bundle directories (ie the search path is a list of
 * parent directories of bundles, not a list of bundle directories).
 */
static void
lilv_world_load_path(LilvWorld*  world,
                     const char* lv2_path)
{
	while (lv2_path[0] != '\0') {
		const char* const sep = first_path_sep(lv2_path);
		if (sep) {
			const size_t dir_len = sep - lv2_path;
			char* const  dir     = malloc(dir_len + 1);
			memcpy(dir, lv2_path, dir_len);
			dir[dir_len] = '\0';
			lilv_world_load_directory(world, dir);
			free(dir);
			lv2_path += dir_len + 1;
		} else {
			lilv_world_load_directory(world, lv2_path);
			lv2_path = "\0";
		}
	}
}

static void
lilv_world_load_specifications(LilvWorld* world)
{
	for (LilvSpec* spec = world->specs; spec; spec = spec->next) {
		LILV_FOREACH(nodes, f, spec->data_uris) {
			LilvNode*      file     = lilv_collection_get(spec->data_uris, f);
			const uint8_t* file_uri = (const uint8_t*)lilv_node_as_uri(file);
			SerdEnv*       env      = serd_env_new(
				sord_node_to_serd_node(file->val.uri_val));
			SerdReader*    reader   = sord_new_reader(world->model, env,
			                                          SERD_TURTLE, NULL);
			serd_reader_add_blank_prefix(reader,
			                             lilv_world_blank_node_prefix(world));
			serd_reader_read_file(reader, file_uri);
			serd_reader_free(reader);
			serd_env_free(env);
		}
	}
}

static void
lilv_world_load_plugin_classes(LilvWorld* world)
{
	/* FIXME: This loads all classes, not just lv2:Plugin subclasses.
	   However, if the host gets all the classes via lilv_plugin_class_get_children
	   starting with lv2:Plugin as the root (which is e.g. how a host would build
	   a menu), they won't be seen anyway...
	*/

	SordIter* classes = lilv_world_find_statements(
		world, world->model,
		NULL,
		world->rdf_a_node,
		world->rdfs_class_node,
		NULL);
	FOREACH_MATCH(classes) {
		const SordNode* class_node = lilv_match_subject(classes);

		// Get parents (superclasses)
		SordIter* parents = lilv_world_find_statements(
			world, world->model,
			class_node,
			world->rdfs_subclassof_node,
			NULL,
			NULL);

		if (lilv_matches_end(parents)) {
			lilv_match_end(parents);
			continue;
		}

		const SordNode* parent_node = lilv_match_object(parents);
		lilv_match_end(parents);

		if (!sord_node_get_type(parent_node) == SORD_URI) {
			// Class parent is not a resource, ignore (e.g. owl restriction)
			continue;
		}

		// Get labels
		SordIter* labels = lilv_world_find_statements(
			world, world->model,
			class_node,
			world->rdfs_label_node,
			NULL,
			NULL);

		if (lilv_matches_end(labels)) {
			lilv_match_end(labels);
			continue;
		}

		const SordNode* label_node = lilv_match_object(labels);
		const uint8_t*  label      = sord_node_get_string(label_node);
		lilv_match_end(labels);

		LilvPluginClasses* classes = world->plugin_classes;
		LilvPluginClass*   pclass  = lilv_plugin_class_new(
			world, parent_node, class_node, (const char*)label);

		if (pclass) {
			zix_tree_insert(classes, pclass, NULL);
		}
	}
	lilv_match_end(classes);
}

LILV_API
void
lilv_world_load_all(LilvWorld* world)
{
	const char* lv2_path = getenv("LV2_PATH");
	if (!lv2_path)
		lv2_path = LILV_DEFAULT_LV2_PATH;

	// Discover bundles and read all manifest files into model
	lilv_world_load_path(world, lv2_path);

	LILV_FOREACH(plugins, p, world->plugins) {
		const LilvPlugin* plugin     = lilv_collection_get(world->plugins, p);
		const LilvNode*   plugin_uri = lilv_plugin_get_uri(plugin);

		// ?new dc:replaces plugin
		SordIter* replacement = lilv_world_find_statements(
			world, world->model,
			NULL,
			world->dc_replaces_node,
			lilv_node_as_node(plugin_uri),
			NULL);
		if (!sord_iter_end(replacement)) {
			/* TODO: Check if replacement is actually a known plugin,
			   though this is expensive...
			*/
			((LilvPlugin*)plugin)->replaced = true;
		}
		lilv_match_end(replacement);
	}

	// Query out things to cache
	lilv_world_load_specifications(world);
	lilv_world_load_plugin_classes(world);
}

LILV_API
const LilvPluginClass*
lilv_world_get_plugin_class(const LilvWorld* world)
{
	return world->lv2_plugin_class;
}

LILV_API
const LilvPluginClasses*
lilv_world_get_plugin_classes(const LilvWorld* world)
{
	return world->plugin_classes;
}

LILV_API
const LilvPlugins*
lilv_world_get_all_plugins(const LilvWorld* world)
{
	return world->plugins;
}
