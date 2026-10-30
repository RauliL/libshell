#ifndef NODES_FREE_H
#define NODES_FREE_H

#include <vector>

union node;

void freenode(union node *n);
void freenodelist(std::vector<union node *> &list);

#endif
