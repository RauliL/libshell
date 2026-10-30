#include "nodes_free.h"

#include "nodes.h"

void freenodelist(std::vector<union node *> &list)
{
	for (union node *n : list)
		freenode(n);
	list.clear();
}

static void free_narg_chain(union node *n)
{
	while (n) {
		union node *next = n->narg.next;
		freenodelist(n->narg.backquote);
		delete reinterpret_cast<struct narg *>(n);
		n = next;
	}
}

void freenode(union node *n)
{
	if (!n)
		return;

	switch (n->type) {
	case NCMD: {
		free_narg_chain(n->ncmd.assign);
		free_narg_chain(n->ncmd.args);
		freenode(n->ncmd.redirect);
		delete reinterpret_cast<struct ncmd *>(n);
		return;
	}
	case NPIPE: {
		freenodelist(n->npipe.cmdlist);
		delete reinterpret_cast<struct npipe *>(n);
		return;
	}
	case NREDIR:
	case NBACKGND:
	case NSUBSHELL:
		freenode(n->nredir.n);
		freenode(n->nredir.redirect);
		delete reinterpret_cast<struct nredir *>(n);
		return;
	case NAND:
	case NOR:
	case NSEMI:
	case NWHILE:
	case NUNTIL:
		freenode(n->nbinary.ch1);
		freenode(n->nbinary.ch2);
		delete reinterpret_cast<struct nbinary *>(n);
		return;
	case NIF:
		freenode(n->nif.test);
		freenode(n->nif.ifpart);
		freenode(n->nif.elsepart);
		delete reinterpret_cast<struct nif *>(n);
		return;
	case NFOR:
		free_narg_chain(n->nfor.args);
		freenode(n->nfor.body);
		delete reinterpret_cast<struct nfor *>(n);
		return;
	case NCASE:
		freenode(n->ncase.expr);
		freenode(n->ncase.cases);
		delete reinterpret_cast<struct ncase *>(n);
		return;
	case NCLIST:
		freenode(n->nclist.next);
		free_narg_chain(n->nclist.pattern);
		freenode(n->nclist.body);
		delete reinterpret_cast<struct nclist *>(n);
		return;
	case NDEFUN:
		freenode(n->ndefun.body);
		delete reinterpret_cast<struct ndefun *>(n);
		return;
	case NARG:
		free_narg_chain(n);
		return;
	case NTO:
	case NCLOBBER:
	case NFROM:
	case NFROMTO:
	case NAPPEND:
		freenode(n->nfile.next);
		freenode(n->nfile.fname);
		delete reinterpret_cast<struct nfile *>(n);
		return;
	case NTOFD:
	case NFROMFD:
		freenode(n->ndup.next);
		freenode(n->ndup.vname);
		delete reinterpret_cast<struct ndup *>(n);
		return;
	case NHERE:
	case NXHERE:
		freenode(n->nhere.next);
		freenode(n->nhere.doc);
		delete reinterpret_cast<struct nhere *>(n);
		return;
	case NNOT:
		freenode(n->nnot.com);
		delete reinterpret_cast<struct nnot *>(n);
		return;
	default:
		break;
	}
}
