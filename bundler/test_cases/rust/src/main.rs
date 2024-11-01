mod point;
mod submod;

use point::Point;
use std::cell::LazyCell;
use submod::test::TestStruct;

/// RealParam
const FOO: LazyCell<f32> = LazyCell::new(|| 42.0);

fn main() {
    println!("FOO={}", *FOO);

    let p = Point { x: 1, y: 2 };
    println!("{:?}", p);

    let t = TestStruct { x: 42 };
    println!("{:?}", t);
}
