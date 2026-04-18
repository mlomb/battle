mod point;
mod submod;

use point::Point;
use std::cell::LazyCell;
use submod::test::TestStruct;

/// RealParam
const FOO: LazyCell<f32> = LazyCell::new(|| 42.0);

/// parameter min=0 max=20
const BAR: LazyCell<[i32; 3]> = LazyCell::new(|| [1, 2, 3]);

fn main() {
    println!("FOO={:?}", *FOO);
    println!("BAR={:?}", *BAR);

    let p = Point { x: 1, y: 2 };
    println!("{:?}", p);

    let t = TestStruct { x: 42 };
    println!("{:?}", t);
}
