//! Prints the C header for the engine: `cargo run -q -p bada-engine --example header > engine/include/bada.h`.

fn main() {
    print!("{}", bada_compat::header("bada"));
}
