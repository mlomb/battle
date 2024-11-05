#include <iostream>

// should be included only once
#include "./point.h"
#include "point.h"
#include "./point.h"

// RealParam
const float FOO = 42.0;


template<typename T>
T duplicate(T n)
{
    
    std::cout << "duplicate" << std::endl;
    return n * 2;
}

const float asd = duplicate(2);

int main() {
    std::cout << "FOO: " << FOO << std::endl;
    std::cout << "asd: " << asd << std::endl;

    Point p;
    p.x = 1;
    p.y = 2;

    std::cout << "Point: (" << p.x << ", " << p.y << ")" << std::endl;

    return 0;
}
