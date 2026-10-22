#ifndef PARSE_CONFIG_H
#define PARSE_CONFIG_H

const int MAX_NODES = 26;

bool parseConfig(const char *filename, int cost[MAX_NODES][MAX_NODES], int &n);

#endif