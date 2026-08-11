use std::env;
use std::error::Error;
use std::ffi::OsStr;
use std::path::PathBuf;

use bindgen::MacroTypeVariation;
use bindgen::callbacks::ParseCallbacks;
use walkdir::WalkDir;

const C_INCLUDE_RELATIVE_DIRECTORY: &str = "c/include";

const C_SOURCE_RELATIVE_DIRECTORY: &str = "c/src";

fn main() -> Result<(), Box<dyn Error>> {
    let current_dir = env::current_dir()?.canonicalize()?;

    let (include, source) = (
        current_dir.join(C_INCLUDE_RELATIVE_DIRECTORY),
        current_dir.join(C_SOURCE_RELATIVE_DIRECTORY),
    );

    let mut build_context = cc::Build::new();

    build_context.include(&include);

    for target_value in WalkDir::new(source) {
        let target_value = target_value?;

        let file_type = target_value.file_type();

        let is_candidate = file_type.is_file() || file_type.is_symlink();

        if is_candidate
            && let Some("C" | "c") = target_value.path().extension().and_then(OsStr::to_str)
        {
            build_context.file(target_value.path());
        }
    }

    build_context.compile(env::var("CARGO_PKG_NAME")?.as_str());

    let mut bind_context =
        bindgen::Builder::default().parse_callbacks(Box::new(bindgen::CargoCallbacks::new()));

    // Track canonical header targets as well as the include-tree symlinks. Cargo otherwise observes
    // the symlink metadata and can miss edits made to Mirilla headers behind that directory link.
    for target_value in WalkDir::new(&include).follow_links(true) {
        let target_value = target_value?;

        if target_value.file_type().is_file() {
            println!(
                "cargo:rerun-if-changed={}",
                target_value.path().canonicalize()?.display()
            );
        }
    }

    for target_value in WalkDir::new(include) {
        let target_value = target_value?;

        let file_type = target_value.file_type();

        let is_candidate = file_type.is_file() || file_type.is_symlink();

        if is_candidate
            && let Some("H" | "h") = target_value.path().extension().and_then(OsStr::to_str)
            && let Some(target_value) = target_value.path().to_str().map(str::to_string)
        {
            bind_context = bind_context.header(target_value);
        }
    }

    bind_context
        .prepend_enum_name(false)
        .default_macro_constant_type(MacroTypeVariation::Signed)
        .parse_callbacks(Box::new(MirillaCallbacks))
        // NOTE: Pass a `__BINDGEN__` preprocessor flag to be able to work around `bindgen` limitations.
        .clang_arg("-D__BINDGEN__")
        .use_core()
        .generate()?
        .write_to_file(PathBuf::from(env::var("OUT_DIR")?).join("catalejo-binding.rs"))?;

    Ok(())
}

/// A [`ParseCallbacks`] for directing specific bindgen behavior.
#[derive(Debug, Copy, Clone)]
pub struct MirillaCallbacks;

impl ParseCallbacks for MirillaCallbacks {
    fn int_macro(&self, name: &str, _: i64) -> Option<bindgen::callbacks::IntKind> {
        // NOTE: The `outside_list` attribute integer macros should be all unsigned to match
        // the actual type alias of a full attribute set.
        if name.starts_with("MIRILLA_OUTSIDE_LIST_ATTRIBUTE_")
            || name.starts_with("MIRILLA_MAP_LAYOUT_ATTRIBUTE_")
        {
            Some(bindgen::callbacks::IntKind::U32)
        } else {
            None
        }
    }
}
