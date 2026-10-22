#include "parse_config.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace std;

bool parseConfig(const char *filename, int cost[MAX_NODES][MAX_NODES], int &n) {
    ifstream fin(filename);
    if (!fin) {
        cerr << "Cannot open config file: " << filename << '\n';
        return false;
    }

    vector<vector<int>> rows;
    string line;
    int line_number = 0;
    while (getline(fin, line)) {
        ++line_number;
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == string::npos || line[first] == '#') continue;

        istringstream iss(line);
        vector<int> values;
        int value;
        while (iss >> value) values.push_back(value);
        if (!iss.eof()) {
            cerr << "Invalid value on config line " << line_number << '\n';
            return false;
        }
        rows.push_back(values);
        if (rows.size() >= MAX_NODES) {
            cerr << "Config file specifies more than " << MAX_NODES << " nodes\n";
            return false;
        }
    }
    if (fin.bad()) {
        cerr << "Error reading config file: " << filename << '\n';
        return false;
    }

    n = static_cast<int>(rows.size()) + 1;
    if (n > MAX_NODES) {
        cerr << "Config file specifies more than " << MAX_NODES << " nodes\n";
        return false;
    }

    for (int i = 0; i < MAX_NODES; ++i) {
        for (int j = 0; j < MAX_NODES; ++j) cost[i][j] = -1;
        cost[i][i] = 0;
    }

    for (int i = 0; i < n - 1; ++i) {
        int expected = n - i - 1;
        if (static_cast<int>(rows[i].size()) != expected) {
            cerr << "Config line for node " << static_cast<char>('A' + i)
                 << " must contain " << expected << " costs\n";
            return false;
        }
        for (int offset = 0; offset < expected; ++offset) {
            int j = i + offset + 1;
            int link_cost = rows[i][offset];
            if (link_cost != -1 && (link_cost <= 0 || link_cost > 65535)) {
                cerr << "Link costs must be between 1 and 65535, or -1 (line "
                     << (i + 1) << ")\n";
                return false;
            }
            cost[i][j] = link_cost;
            cost[j][i] = link_cost;
        }
    }

    return true;
}
