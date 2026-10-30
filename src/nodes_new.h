#ifndef NODES_NEW_H
#define NODES_NEW_H

#include "nodes.h"

/* Allocate a typed parse node with new; freenode() deletes by type. */
template <typename T>
inline union node *new_node()
{
	return reinterpret_cast<union node *>(new T());
}

#endif
