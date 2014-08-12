#include <facter/ruby/module.hpp>
#include <facter/ruby/api.hpp>
#include <facter/ruby/aggregate_resolution.hpp>
#include <facter/ruby/confine.hpp>
#include <facter/ruby/simple_resolution.hpp>
#include <facter/facts/collection.hpp>
#include <facter/logging/logging.hpp>
#include <facter/util/directory.hpp>
#include <facter/util/string.hpp>
#include <facter/execution/execution.hpp>
#include <facter/version.h>
#include <boost/filesystem.hpp>
#include <stdexcept>

using namespace std;
using namespace facter::facts;
using namespace facter::util;
using namespace facter::execution;
using namespace facter::logging;
using namespace boost::filesystem;

LOG_DECLARE_NAMESPACE("ruby");

/**
 * Helper for maintaining context when initialized via the Ruby gem.
 */
struct ruby_context
{
    /**
     * Constructs a new Ruby context.
     */
    ruby_context()
    {
        // Create a collection and a Ruby module
        _facts.reset(new collection());
        _module.reset(new facter::ruby::module(*_facts));

        // Ruby doesn't have a proper way of notifying extensions that the VM is shutting down
        // The easiest way to get notified is to have a global data object that never gets collected
        // until the VM shuts down
        auto const& ruby = *facter::ruby::api::instance();
        _canary = ruby.rb_data_object_alloc(*ruby.rb_cObject, this, nullptr, cleanup);
        ruby.rb_gc_register_address(&_canary);
    }

    /**
     * Destructs the Ruby context.
     */
    ~ruby_context()
    {
        release();
    }

    /**
     * Releases the Ruby context.
     */
    void release()
    {
        _module.reset();
        _facts.reset();

        // Unregister the canary; the context will be deleted on next GC or VM shutdown
        auto const& ruby = *facter::ruby::api::instance();
        ruby.rb_gc_unregister_address(&_canary);
    }

 private:
    static void cleanup(void* ptr)
    {
        ruby_context* instance = reinterpret_cast<ruby_context*>(ptr);
        delete instance;
    }

    unique_ptr<collection> _facts;
    unique_ptr<facter::ruby::module> _module;
    facter::ruby::VALUE _canary;
};

ruby_context* g_context = nullptr;

// Exports for the Ruby cfacter gem.
extern "C" {
    /**
     * Gets the cfacter gem version.
     */
    char const* facter_version()
    {
        return LIBFACTER_VERSION;
    }

    /**
     * Initializes the cfacter gem.
     * @param level The logging level to use.
     */
    void initialize_facter(unsigned int level)
    {
        // Start by configuring logging
        configure_logging(static_cast<log_level>(level), std::cerr);

        // Initialize ruby
        auto ruby = facter::ruby::api::instance();
        if (!ruby) {
            return;
        }
        ruby->initialize();

        // The lifetime of the context object is tied to Ruby VM
        g_context = new ruby_context();
    }

    /**
     * Shuts down the cfacter gem.
     */
    void shutdown_facter()
    {
        if (g_context) {
            // Just reset the context; it will be deleted by the Ruby VM
            g_context->release();
            g_context = nullptr;
        }
    }
}

namespace facter { namespace ruby {

    template struct object<module>;

    module::module(collection& facts, vector<string> const& paths) :
        _collection(facts),
        _loaded_all(false)
    {
        if (!api::instance()) {
            throw runtime_error("Ruby API is not present.");
        }
        auto const& ruby = *api::instance();
        if (!ruby.initialized()) {
            throw runtime_error("Ruby API is not initialized.");
        }

        // Initialize the search paths
        initialize_search_paths(paths);

        // Undefine Facter if it's already defined
        ruby.rb_gc_register_address(&_previous_facter);
        if (ruby.is_true(ruby.rb_const_defined(*ruby.rb_cObject, ruby.rb_intern("Facter")))) {
            _previous_facter = ruby.rb_const_remove(*ruby.rb_cObject, ruby.rb_intern("Facter"));
        } else {
            _previous_facter = ruby.nil_value();
        }

        // Define the Facter module
        self(ruby.rb_define_module("Facter"));
        VALUE facter = self();

        VALUE core = ruby.rb_define_module_under(facter, "Core");
        VALUE execution = ruby.rb_define_module_under(core, "Execution");
        ruby.rb_define_module_under(facter, "Util");

        // Define the methods on the Facter module
        volatile VALUE version = ruby.rb_str_new_cstr(LIBFACTER_VERSION);
        ruby.rb_const_set(facter, ruby.rb_intern("CFACTERVERSION"), version);
        ruby.rb_const_set(facter, ruby.rb_intern("FACTERVERSION"), version);
        ruby.rb_define_singleton_method(facter, "version", RUBY_METHOD_FUNC(ruby_version), 0);
        ruby.rb_define_singleton_method(facter, "add", RUBY_METHOD_FUNC(ruby_add), -1);
        ruby.rb_define_singleton_method(facter, "define_fact", RUBY_METHOD_FUNC(ruby_define_fact), -1);
        ruby.rb_define_singleton_method(facter, "value", RUBY_METHOD_FUNC(ruby_value), 1);
        ruby.rb_define_singleton_method(facter, "[]", RUBY_METHOD_FUNC(ruby_fact), 1);
        ruby.rb_define_singleton_method(facter, "fact", RUBY_METHOD_FUNC(ruby_fact), 1);
        ruby.rb_define_singleton_method(facter, "debug", RUBY_METHOD_FUNC(ruby_debug), 1);
        ruby.rb_define_singleton_method(facter, "debugonce", RUBY_METHOD_FUNC(ruby_debugonce), 1);
        ruby.rb_define_singleton_method(facter, "warn", RUBY_METHOD_FUNC(ruby_warn), 1);
        ruby.rb_define_singleton_method(facter, "warnonce", RUBY_METHOD_FUNC(ruby_warnonce), 1);
        ruby.rb_define_singleton_method(facter, "log_exception", RUBY_METHOD_FUNC(ruby_log_exception), -1);
        ruby.rb_define_singleton_method(facter, "flush", RUBY_METHOD_FUNC(ruby_flush), 0);
        ruby.rb_define_singleton_method(facter, "list", RUBY_METHOD_FUNC(ruby_list), 0);
        ruby.rb_define_singleton_method(facter, "to_hash", RUBY_METHOD_FUNC(ruby_to_hash), 0);
        ruby.rb_define_singleton_method(facter, "each", RUBY_METHOD_FUNC(ruby_each), 0);
        ruby.rb_define_singleton_method(facter, "clear", RUBY_METHOD_FUNC(ruby_clear), 0);
        ruby.rb_define_singleton_method(facter, "reset", RUBY_METHOD_FUNC(ruby_reset), 0);
        ruby.rb_define_singleton_method(facter, "loadfacts", RUBY_METHOD_FUNC(ruby_loadfacts), 0);
        ruby.rb_define_singleton_method(facter, "search", RUBY_METHOD_FUNC(ruby_search), -1);
        ruby.rb_define_singleton_method(facter, "search_path", RUBY_METHOD_FUNC(ruby_search_path), 0);
        ruby.rb_define_singleton_method(facter, "search_external", RUBY_METHOD_FUNC(ruby_search_external), 1);
        ruby.rb_define_singleton_method(facter, "search_external_path", RUBY_METHOD_FUNC(ruby_search_external_path), 0);

        // Define the execution module
        ruby.rb_define_singleton_method(execution, "which", RUBY_METHOD_FUNC(ruby_which), 1);
        ruby.rb_define_singleton_method(execution, "exec", RUBY_METHOD_FUNC(ruby_exec), 1);
        ruby.rb_define_singleton_method(execution, "execute", RUBY_METHOD_FUNC(ruby_execute), -1);
        ruby.rb_define_class_under(execution, "ExecutionFailure", *ruby.rb_eStandardError);
        ruby.rb_obj_freeze(execution);

        // Define the Fact and resolution classes
        fact::define();
        simple_resolution::define();
        aggregate_resolution::define();

        // To prevent custom facts from including parts of Ruby Facter and overriding the above definitions,
        // grab the first directory on the load path and append certain files to $LOADED_FEATURES
        volatile VALUE first = ruby.rb_ary_entry(ruby.rb_gv_get("$LOAD_PATH"), 0);
        if (!ruby.is_nil(first)) {
            volatile VALUE features = ruby.rb_gv_get("$LOADED_FEATURES");
            path p = ruby.to_string(first);
            ruby.rb_ary_push(features, ruby.rb_str_new_cstr((p / "facter.rb").string().c_str()));
            ruby.rb_ary_push(features, ruby.rb_str_new_cstr((p / "facter" / "util" / "resolution.rb").string().c_str()));
            ruby.rb_ary_push(features, ruby.rb_str_new_cstr((p / "facter" / "core" / "aggregate.rb").string().c_str()));
            ruby.rb_ary_push(features, ruby.rb_str_new_cstr((p / "facter" / "core" / "execution.rb").string().c_str()));
        }
    }

    module::~module()
    {
        clear_facts(false);

        auto ruby = api::instance();
        if (!ruby) {
            // Ruby has been uninitialized
            return;
        }

        // Undefine the module and restore the previous value
        ruby->rb_const_remove(*ruby->rb_cObject, ruby->rb_intern("Facter"));
        if (!ruby->is_nil(_previous_facter)) {
            ruby->rb_const_set(*ruby->rb_cObject, ruby->rb_intern("Facter"), _previous_facter);
        }

        ruby->rb_gc_unregister_address(&_previous_facter);
    }

    void module::load_facts()
    {
        if (_loaded_all) {
            return;
        }

        LOG_DEBUG("loading all custom facts.");

        for (auto const& directory : _search_paths) {
            LOG_DEBUG("searching for custom facts in %1%.", directory);
            directory::each_file(directory, [&](string const& file) {
                load_file(file);
                return true;
            }, "\\.rb$");
        }

        _loaded_all = true;
    }

    void module::resolve_facts()
    {
        // Before we do anything, call facts to ensure the collection is populated
        facts();

        load_facts();

        // Get the value from all facts
        for (auto const& kvp : _facts) {
            fact::from_self(kvp.second)->value();
        }
    }

    void module::clear_facts(bool clear_collection)
    {
        auto const& ruby = *api::instance();

        // Unregister all the facts
        for (auto& kvp : _facts) {
            ruby.rb_gc_unregister_address(&kvp.second);
        }

        // Clear the custom facts
        _facts.clear();

        // Clear the collection
        if (clear_collection) {
            _collection.clear();
        }
    }

    VALUE module::fact_value(VALUE name)
    {
        auto const& ruby = *api::instance();

        VALUE fact_self = load_fact(name);
        if (ruby.is_nil(fact_self)) {
            return ruby.nil_value();
        }

        return fact::from_self(fact_self)->value();
    }

    VALUE module::normalize(VALUE name) const
    {
        auto const& ruby = *api::instance();

        if (ruby.is_symbol(name)) {
            name = ruby.rb_sym_to_s(name);
        }
        if (ruby.is_string(name)) {
            name = ruby.rb_funcall(name, ruby.rb_intern("downcase"), 0);
        }
        return name;
    }

    collection& module::facts()
    {
        if (_collection.empty()) {
            _collection.add_default_facts();
            _collection.add_external_facts(_external_search_paths);
        }
        return _collection;
    }

    VALUE module::ruby_version(VALUE self)
    {
        auto const& ruby = *api::instance();
        return ruby.lookup({ "Facter", "FACTERVERSION" });
    }

    VALUE module::ruby_add(int argc, VALUE* argv, VALUE self)
    {
        auto const& ruby = *api::instance();

        if (argc == 0 || argc > 2) {
            ruby.rb_raise(*ruby.rb_eArgError, "wrong number of arguments (%d for 2)", argc);
        }

        fact* f = fact::from_self(from_self(self)->create_fact(argv[0]));

        // Read the resolution name from the options hash, if present
        VALUE name = ruby.nil_value();
        VALUE options = argc == 2 ? argv[1] : ruby.nil_value();
        if (!ruby.is_nil(options)) {
            name = ruby.rb_funcall(
                    options,
                    ruby.rb_intern("delete"),
                    1,
                    ruby.rb_funcall(ruby.rb_str_new_cstr("name"), ruby.rb_intern("to_sym"), 0));
        }

        int tag = 0;
        ruby.protect(tag, [&]() {
            // Define a resolution
            VALUE resolution_self = f->define_resolution(name, options);

            // Call the block if one was given
            if (ruby.rb_block_given_p()) {
                ruby.rb_funcall_passing_block(resolution_self, ruby.rb_intern("instance_eval"), 0, nullptr);
                return ruby.nil_value();
            }
            return ruby.nil_value();
        });

        // If we've failed, set the value to nil
        if (tag) {
            f->value(ruby.nil_value());
            ruby.rb_jump_tag(tag);
        }
        return f->self();
    }

    VALUE module::ruby_define_fact(int argc, VALUE* argv, VALUE self)
    {
        auto const& ruby = *api::instance();

        if (argc == 0 || argc > 2) {
            ruby.rb_raise(*ruby.rb_eArgError, "wrong number of arguments (%d for 2)", argc);
        }

        fact* f = fact::from_self(from_self(self)->create_fact(argv[0]));

        // Call the block if one was given
        if (ruby.rb_block_given_p()) {
            ruby.rb_funcall_passing_block(f->self(), ruby.rb_intern("instance_eval"), 0, nullptr);
        }
        return f->self();
    }

    VALUE module::ruby_value(VALUE self, VALUE name)
    {
        return from_self(self)->fact_value(name);
    }

    VALUE module::ruby_fact(VALUE self, VALUE name)
    {
        return from_self(self)->load_fact(name);
    }

    VALUE module::ruby_debug(VALUE self, VALUE message)
    {
        auto const& ruby = *api::instance();
        LOG_DEBUG(ruby.to_string(message));
        return ruby.nil_value();
    }

    VALUE module::ruby_debugonce(VALUE self, VALUE message)
    {
        auto const& ruby = *api::instance();

        string msg = ruby.to_string(message);
        if (from_self(self)->_debug_messages.insert(msg).second) {
            LOG_DEBUG(msg);
        }
        return ruby.nil_value();
    }

    VALUE module::ruby_warn(VALUE self, VALUE message)
    {
        auto const& ruby = *api::instance();
        LOG_WARNING(ruby.to_string(message));
        return ruby.nil_value();
    }

    VALUE module::ruby_warnonce(VALUE self, VALUE message)
    {
        auto const& ruby = *api::instance();

        string msg = ruby.to_string(message);
        if (from_self(self)->_warning_messages.insert(msg).second) {
            LOG_WARNING(msg);
        }
        return ruby.nil_value();
    }

    VALUE module::ruby_log_exception(int argc, VALUE* argv, VALUE self)
    {
        auto const& ruby = *api::instance();

        if (argc == 0 || argc > 2) {
            ruby.rb_raise(*ruby.rb_eArgError, "wrong number of arguments (%d for 2)", argc);
        }

        LOG_ERROR("%1%.\nbacktrace:\n%2%",
            argc == 1 ? ruby.to_string(argv[0]) : ruby.to_string(argv[1]),
            ruby.exception_backtrace(argv[0]));
        return ruby.nil_value();
    }

    VALUE module::ruby_flush(VALUE self)
    {
        auto const& ruby = *api::instance();

        for (auto& kvp : from_self(self)->_facts)
        {
            fact::from_self(kvp.second)->flush();
        }
        return ruby.nil_value();
    }

    VALUE module::ruby_list(VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        instance->resolve_facts();

        volatile VALUE array = ruby.rb_ary_new_capa(instance->facts().size());

        instance->facts().each([&](string const& name, value const*) {
            ruby.rb_ary_push(array, ruby.rb_str_new_cstr(name.c_str()));
            return true;
        });
        return array;
    }

    VALUE module::ruby_to_hash(VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        instance->resolve_facts();

        volatile VALUE hash = ruby.rb_hash_new();

        instance->facts().each([&](string const& name, value const* val) {
            ruby.rb_hash_aset(hash, ruby.rb_str_new_cstr(name.c_str()), ruby.to_ruby(val));
            return true;
        });
        return hash;
    }

    VALUE module::ruby_each(VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        instance->resolve_facts();

        instance->facts().each([&](string const& name, value const* val) {
            ruby.rb_yield_values(2, ruby.rb_str_new_cstr(name.c_str()), ruby.to_ruby(val));
            return true;
        });
        return self;
    }

    VALUE module::ruby_clear(VALUE self)
    {
        auto const& ruby = *api::instance();

        ruby_flush(self);
        ruby_reset(self);

        return ruby.nil_value();
    }

    VALUE module::ruby_reset(VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        instance->clear_facts();
        instance->initialize_search_paths({});
        instance->_external_search_paths.clear();
        instance->_loaded_all = false;
        instance->_loaded_files.clear();

        return ruby.nil_value();
    }

    VALUE module::ruby_loadfacts(VALUE self)
    {
        auto const& ruby = *api::instance();

        from_self(self)->load_facts();
        return ruby.nil_value();
    }

    VALUE module::ruby_search(int argc, VALUE* argv, VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        for (int i = 0; i < argc; ++i) {
            if (!ruby.is_string(argv[i])) {
                continue;
            }
            instance->_additional_search_paths.emplace_back(ruby.to_string(argv[i]));

            // Get the canonical directory name
            boost::system::error_code ec;
            path directory = canonical(instance->_additional_search_paths.back(), ec);
            if (ec) {
                continue;
            }

            instance->_search_paths.push_back(directory.string());
        }
        return ruby.nil_value();
    }

    VALUE module::ruby_search_path(VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        volatile VALUE array = ruby.rb_ary_new_capa(instance->_additional_search_paths.size());

        for (auto const& path : instance->_additional_search_paths) {
            ruby.rb_ary_push(array, ruby.rb_str_new_cstr(path.c_str()));
        }
        return array;
    }

    VALUE module::ruby_search_external(VALUE self, VALUE paths)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        ruby.array_for_each(paths, [&](VALUE element) {
            if (!ruby.is_string(element)) {
                return true;
            }
            instance->_external_search_paths.emplace_back(ruby.to_string(element));
            return true;
        });
        return ruby.nil_value();
    }

    VALUE module::ruby_search_external_path(VALUE self)
    {
        auto const& ruby = *api::instance();
        module* instance = from_self(self);

        volatile VALUE array = ruby.rb_ary_new_capa(instance->_external_search_paths.size());

        for (auto const& path : instance->_external_search_paths) {
            ruby.rb_ary_push(array, ruby.rb_str_new_cstr(path.c_str()));
        }
        return array;
    }

    VALUE module::ruby_which(VALUE self, VALUE binary)
    {
        // Note: self is Facter::Core::Execution
        auto const& ruby = *api::instance();

        string path = execution::which(ruby.to_string(binary));
        if (path.empty()) {
            return ruby.nil_value();
        }

        return ruby.rb_str_new_cstr(path.c_str());
    }

    VALUE module::ruby_exec(VALUE self, VALUE command)
    {
        // Note: self is Facter::Core::Execution
        auto const& ruby = *api::instance();
        return execute_command(ruby.to_string(command), ruby.nil_value(), false);
    }

    VALUE module::ruby_execute(int argc, VALUE* argv, VALUE self)
    {
        // Note: self is Facter::Core::Execution
        auto const& ruby = *api::instance();

        if (argc == 0 || argc > 2) {
            ruby.rb_raise(*ruby.rb_eArgError, "wrong number of arguments (%d for 2)", argc);
        }

        if (argc == 1) {
            return execute_command(ruby.to_string(argv[0]), ruby.nil_value(), true);
        }

        // Unfortunately we have to call to_sym rather than using ID2SYM, which is Ruby version dependent
        volatile VALUE option = ruby.rb_hash_lookup(argv[1], ruby.rb_funcall(ruby.rb_str_new_cstr("on_fail"), ruby.rb_intern("to_sym"), 0));
        if (ruby.is_symbol(option) && ruby.to_string(option) == "raise") {
            return execute_command(ruby.to_string(argv[0]), ruby.nil_value(), true);
        }
        return execute_command(ruby.to_string(argv[0]), option, false);
    }

    VALUE module::execute_command(std::string const& command, VALUE failure_default, bool raise)
    {
        auto const& ruby = *api::instance();

        // Block to ensure that result is destructed before raising.
        {
            auto result = execution::execute("sh", {"-c", expand_command(command)},
                option_set<execution_options> {
                    execution_options::defaults,
                    execution_options::redirect_stderr
                });
            if (result.first) {
                return ruby.rb_str_new_cstr(result.second.c_str());
            }
        }
        if (raise) {
            ruby.rb_raise(ruby.lookup({ "Facter", "Core", "Execution", "ExecutionFailure"}), "execution of command \"%s\" failed", command.c_str());
        }
        return failure_default;
    }

    void module::initialize_search_paths(vector<string> const& paths)
    {
        auto const& ruby = *api::instance();

        _search_paths.clear();
        _additional_search_paths.clear();

        // Look for "facter" subdirectories on the load path
        for (auto const& directory : ruby.get_load_path()) {
            // Get the canonical directory name
            boost::system::error_code ec;
            path dir = canonical(directory, ec);
            if (ec) {
                continue;
            }

            // Ignore facter itself if it's on the load path
            if (is_regular_file(dir / "facter.rb", ec)) {
                continue;
            }

            dir = dir / "facter";
            if (!is_directory(dir, ec)) {
                continue;
            }
            _search_paths.push_back(dir.string());
        }

        // Append the FACTERLIB paths
        string variable;
        if (environment::get("FACTERLIB", variable)) {
            vector<string> env_paths = split(variable, environment::get_path_separator());
            _search_paths.insert(_search_paths.end(), make_move_iterator(env_paths.begin()), make_move_iterator(env_paths.end()));
        }

        // Insert the given paths last
        _search_paths.insert(_search_paths.end(), paths.begin(), paths.end());

        // Do a canonical transform
        transform(_search_paths.begin(), _search_paths.end(), _search_paths.begin(), [](string const& directory) -> string {
            // Get the canonical directory name
            boost::system::error_code ec;
            path dir = canonical(directory, ec);
            if (ec) {
                LOG_DEBUG("path \"%1%\" will not be searched for custom facts: %2%.", directory, ec.message());
                return {};
            }
            return dir.string();
        });

        // Remove anything that is empty
        remove_if(_search_paths.begin(), _search_paths.end(), [](string const& path) {
            return path.empty();
        });
    }

    VALUE module::load_fact(VALUE name)
    {
        auto const& ruby = *api::instance();

        name = normalize(name);
        string fact_name = ruby.to_string(name);

        // First check to see if we have that fact already
        auto it = _facts.find(fact_name);
        if (it != _facts.end()) {
            return it->second;
        }

        // Try to load it by file name
        if (!_loaded_all) {
            // Next, attempt to load it by file
            string filename = fact_name + ".rb";
            LOG_DEBUG("searching for custom fact \"%1%\".", fact_name);

            for (auto const& directory : _search_paths) {
                LOG_DEBUG("searching for %1% in %2%.", filename, directory);

                // Check to see if there's a file of a matching name in this directory
                path full_path = path(directory) / filename;
                boost::system::error_code ec;
                if (!is_regular_file(full_path, ec)) {
                    continue;
                }

                // Load the fact file
                load_file(full_path.string());
            }

            // Check to see if we now have the fact
            it = _facts.find(fact_name);
            if (it != _facts.end()) {
                return it->second;
            }
        }

        // Otherwise, check to see if it's already in the collection
        auto value = facts()[fact_name];
        if (value) {
            return create_fact(name);
        }

        // Couldn't load the fact by file name, load all facts to try to find it
        load_facts();

        // Check to see if we now have the fact
        it = _facts.find(fact_name);
        if (it != _facts.end()) {
            return it->second;
        }

        // Couldn't find the fact
        LOG_DEBUG("custom fact \"%1%\" was not found.", fact_name);
        return ruby.nil_value();
    }

    void module::load_file(std::string const& path)
    {
        // Only load the file if we haven't done so before
        if (!_loaded_files.insert(path).second) {
            return;
        }

        auto const& ruby = *api::instance();

        LOG_INFO("loading custom facts from %1%.", path);
        ruby.rescue([&]() {
            // Do not construct C++ objects in a rescue callback
            // C++ stack unwinding will not take place if a Ruby exception is thrown!
             ruby.rb_load(ruby.rb_str_new_cstr(path.c_str()), 0);
            return 0;
        }, [&](VALUE ex) {
            LOG_ERROR("error while resolving custom facts in %1%: %2%.\nbacktrace:\n%3%",
                path,
                ruby.to_string(ex),
                ruby.exception_backtrace(ex));
            return 0;
        });
    }

    VALUE module::create_fact(VALUE name)
    {
        auto const& ruby = *api::instance();

        if (!ruby.is_string(name) && !ruby.is_symbol(name)) {
            ruby.rb_raise(*ruby.rb_eTypeError, "expected a String or Symbol for fact name");
        }

        name = normalize(name);
        string fact_name = ruby.to_string(name);

         // First check to see if we have that fact already
        auto it = _facts.find(fact_name);
        if (it == _facts.end()) {
            // Before adding the first fact, call facts to ensure the collection is populated
            facts();
            it = _facts.insert(make_pair(fact_name, fact::create(name))).first;
            ruby.rb_gc_register_address(&it->second);
        }
        return it->second;
    }

}}  // namespace facter::ruby
