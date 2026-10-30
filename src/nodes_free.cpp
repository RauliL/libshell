#include "nodes_free.h"

#include <cstdlib>

#include "nodes.h"

static void free_narg_chain(union node *n);

void freenodelist(struct nodelist *lp)
{
	while (lp) {
		struct nodelist *next = lp->next;
		freenode(lp->n);
		std::free(lp);
		lp = next;
	}
}

static void free_narg_chain(union node *n)
{
	while (n) {
		union node *next = n->narg.next;
		freenodelist(n->narg.backquote);
		std::free(n->narg.text);
		std::free(n);
		n = next;
	}
}

void freenode(union node *n)
{
	if (!n)
		return;

	switch (n->type) {
	case NCMD:
		free_narg_chain(n->ncmd.assign);
		free_narg_chain(n->ncmd.args);
		freenode(n->ncmd.redirect);
		break;
	case NPIPE: {
		struct nodelist *lp = n->npipe.cmdlist;
		while (lp) {
			struct nodelist *next = lp->next;
			freenode(lp->n);
			std::free(lp);
			lp = next;
		}
		break;
	}
	case NREDIR:
	case NBACKGND:
	case NSUBSHELL:
		freenode(n->nredir.n);
		freenode(n->nredir.redirect);
		break;
	case NAND:
	case NOR:
	case NSEMI:
	case NWHILE:
	case NUNTIL:
		freenode(n->nbinary.ch1);
		freenode(n->nbinary.ch2);
		break;
	case NIF:
		freenode(n->nif.test);
		freenode(n->nif.ifpart);
		freenode(n->nif.elsepart);
		break;
	case NFOR:
		free_narg_chain(n->nfor.args);
		freenode(n->nfor.body);
		std::free(n->nfor.var);
		break;
	case NCASE:
		freenode(n->ncase.expr);
		freenode(n->ncase.cases);
		break;
	case NCLIST:
		freenode(n->nclist.next);
		free_narg_chain(n->nclist.pattern);
		freenode(n->nclist.body);
		break;
	case NDEFUN:
		std::free(n->ndefun.text);
		freenode(n->ndefun.body);
		break;
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
		std::free(n->nfile.expfname);
		break;
	case NTOFD:
	case NFROMFD:
		freenode(n->ndup.next);
		freenode(n->ndup.vname);
		break;
	case NHERE:
	case NXHERE:
		freenode(n->nhere.next);
		freenode(n->nhere.doc);
		break;
	case NNOT:
		freenode(n->nnot.com);
		break;
	default:
		break;
	}
	std::free(n);
}
