fn main() {
    // Link against libcorm — the C library must be installed separately.
    // Use CORM_DIR to point to a custom installation (e.g. CORM_DIR=/usr/local).
    if let Ok(dir) = std::env::var("CORM_DIR") {
        println!("cargo:rustc-link-search={}/lib", dir);
    }
    println!("cargo:rustc-link-lib=corm");

    // Rebuild when this script or CORM_DIR changes.
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=CORM_DIR");
}
