#ifndef NODES_FREE_H
#define NODES_FREE_H

union node;
struct nodelist;

void freenode(union node *n);
void freenodelist(struct nodelist *lp);

#endif
