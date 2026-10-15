#include <iostream>
#include <vector>
#include <cmath>   
using namespace std;

std::vector<float> audioSamples( float frequency , float sampleRate , float duration , float amplitude){
    std::vector<float> ansSamples ;

    for(float i = 0 ; i<10 ; i++){
        float value = std::sin(2*3.14159*frequency*(i/sampleRate)) * amplitude ;
        ansSamples.push_back(value *32767);

    }
    return ansSamples;
}

int main() {

    for(float x : audioSamples(440, 44100, 1, 0.5)){
        std::cout << x << " ";
    } ;       
        return 0;
}   