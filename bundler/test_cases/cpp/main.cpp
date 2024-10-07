#include <iostream>

// should be included only once
#include "./point.h"
#include "point.h"
#include "./point.h"

int main() {
    Point p;
    p.x = 1;
    p.y = 2;

    std::cout << "Point: (" << p.x << ", " << p.y << ")" << std::endl;

    return 0;
}
