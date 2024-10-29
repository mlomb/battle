mod point;

use point::Point;
use std::cell::LazyCell;

/// RealParam
const FOO: LazyCell<f32> = LazyCell::new(|| 42.0);

fn main() {
    println!("FOO={}", *FOO);

    let p = Point { x: 1, y: 2 };
    println!("{:?}", p);
}
