mod point;

use point::Point;

fn main() {
    let p = Point { x: 1, y: 2 };

    println!("{:?}", p);
}
