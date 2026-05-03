#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace std;

const std::vector<string> actions = {"LEFT", "RIGHT", "UP", "DOWN"};

int main()
{
    int player_idx;
    cin >> player_idx; cin.ignore();
    int nb_games;
    cin >> nb_games; cin.ignore();

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> pick(0, actions.size() - 1);

    // game loop
    while (1) {
        for (int i = 0; i < 3; i++) {
            string score_info;
            getline(cin, score_info);
        }
        for (int i = 0; i < nb_games; i++) {
            string gpu;
            int reg0;
            int reg1;
            int reg2;
            int reg3;
            int reg4;
            int reg5;
            int reg6;
            cin >> gpu >> reg0 >> reg1 >> reg2 >> reg3 >> reg4 >> reg5 >> reg6; cin.ignore();
        }

        // take random action
        cout << actions[pick(gen)] << endl;
    }
}