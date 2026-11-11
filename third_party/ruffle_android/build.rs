// The bridge source is the iOS crate's lib.rs. It's copied into OUT_DIR with
// the two things that are Metal's swapped: where the surface comes from and
// which wgpu backends are asked for.

use std::{env, fs, path::PathBuf};

fn swap(text: &str, from: &str, to: &str, times: usize) -> String {
    let found = text.matches(from).count();
    assert_eq!(found, times, "expected {times} of `{from}` in the iOS bridge, found {found}");
    text.replace(from, to)
}

fn main() {
    let ios = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("../../ios/third_party/ruffle_ios");
    let lib = ios.join("src/lib.rs");
    let manifest = ios.join("Cargo.toml");
    println!("cargo:rerun-if-changed={}", lib.display());
    println!("cargo:rerun-if-changed={}", manifest.display());

    let rev = |text: &str| {
        text.lines()
            .find(|l| l.starts_with("ruffle_core"))
            .and_then(|l| l.split("rev = \"").nth(1))
            .and_then(|r| r.split('"').next())
            .map(str::to_owned)
    };
    let theirs = rev(&fs::read_to_string(&manifest).unwrap());
    let ours = rev(&fs::read_to_string("Cargo.toml").unwrap());
    assert_eq!(theirs, ours, "Cargo.toml's ruffle rev is behind the iOS crate's");

    let text = fs::read_to_string(&lib).unwrap();
    let text = swap(&text, "#![allow(clippy::missing_safety_doc)]", "", 1);
    let text = swap(
        &text,
        "wgpu::SurfaceTargetUnsafe::CoreAnimationLayer(layer_ptr)",
        "crate::window_surface_target(layer_ptr)",
        3,
    );
    let text = swap(&text, "wgpu::Backends::METAL", "crate::BACKENDS", 4);
    let out = PathBuf::from(env::var("OUT_DIR").unwrap()).join("bridge.rs");
    fs::write(out, text).unwrap();
}
