// Compiles ui/app.slint to Rust at build time (host-side) and embeds fonts and
// resources in the format the software renderer consumes. `EmbedForSoftwareRenderer`
// is what makes Slint emit pre-rasterised bitmap fonts instead of requiring a
// system font at runtime — essential for a no_std target.
fn main() {
    slint_build::compile_with_config(
        "ui/app.slint",
        slint_build::CompilerConfiguration::new()
            .embed_resources(slint_build::EmbedResourcesKind::EmbedForSoftwareRenderer),
    )
    .unwrap();
}
