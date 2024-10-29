mod point;

use point::Point;
use std::cell::LazyCell;

/// RealParam
const FOO: LazyCell<f32> = LazyCell::new(|| 42.0);

// should be converted to ↓

const FOO_: LazyCell<f32> = LazyCell::new(|| {
    std::env::args()
        .skip(1)
        .collect::<Vec<_>>()
        .chunks_exact(2)
        .find(|item| item[0] == "FOO")
        .map(|item| item[1].parse().unwrap())
        .unwrap_or_else(|| 42.0)
});

fn main() {
    println!("FOO={}", *FOO);
    println!("FOO(arg)={}", *FOO_);

    let p = Point { x: 1, y: 2 };
    println!("{:?}", p);
}
