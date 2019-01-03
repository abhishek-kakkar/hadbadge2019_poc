/*
 This program converts one or multiple .tmx files (as generated by Tiled) to C includes to be used by the tilegfx
 component of the PocketSprite SDK. It is fairly limited; please refer to the documentation to read what it can and
 cannot do.

 (And yes, this piece of code is an unstructured ball of mud. Sorry for that.)
*/
#include <stdio.h>
#include <stdlib.h>
#include <gd.h>
#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdint.h>
#include <string.h>

FILE *cfile, *hfile;
char *basedir;
char *filebase;
int bytestotal;

typedef struct tmx_llitem_t tmx_llitem_t;
struct tmx_llitem_t {
	char *tmxname;
	char *tilename;
	tmx_llitem_t *next;
};

tmx_llitem_t *tmxinfo=NULL;

int conv_to_8bit(int r, int g, int b) {
	int ret=0;
	if (r&0x80) ret|=0x80;
	if (r&0x40) ret|=0x40;
	if (r&0x20) ret|=0x20;
	if (g&0x80) ret|=0x10;
	if (g&0x40) ret|=0x08;
	if (g&0x20) ret|=0x04;
	if (b&0x80) ret|=0x02;
	if (b&0x40) ret|=0x01;
	return ret;
}

void conv_to_c_ident(char *str) {
	char *p=str;
	while (*p!=0) {
		if (*p==' ') *p='_';
		if (*p=='-') *p='_';
		if (*p==':') *p='_';
		p++;
	}
}

xmlNodePtr findNodeByName(xmlNodePtr parent, const xmlChar *name) {
	if (!parent) return NULL;
	xmlNodePtr node=parent->children;
	while (node) {
		if (!xmlStrcmp(node->name, name)) {
			return node;
		}
		node=node->next;
	}
	return NULL;
}

int output_tileset(FILE *f, char *name, int trans_col, int has_anim) {
	gdImagePtr im=gdImageCreateFromPng(f);
	if (im==NULL) goto err;
	int h=gdImageSY(im)/8;
	int w=gdImageSX(im)/8;
	fprintf(hfile, "extern const tilegfx_tileset_t tileset_%s;\n", name);
	fprintf(cfile, "\nconst tilegfx_tileset_t tileset_%s={ //%d tiles\n", name, w*h);
	if (trans_col==-1) {
		fprintf(cfile, "\t.trans_col=-1; //No transparency\n");
	} else {
		fprintf(cfile, "\t.trans_col=0x%02X,\n", trans_col);
	}
	if (has_anim) {
		fprintf(cfile, "\t.anim_offsets=%s_anim_offsets,\n", name);
		fprintf(cfile, "\t.anim_frames=%s_anim_frames,\n", name);
	} else {
		fprintf(cfile, "\t.anim_offsets=NULL,\n");
		fprintf(cfile, "\t.anim_frames=NULL,\n");
	}
	fprintf(cfile, "\t.tile={");
	for (int y=0; y<h; y++) {
		for (int x=0; x<w; x++) {
			fprintf(cfile, "\n\t\t");
			for (int yy=0; yy<8; yy++) {
				for (int xx=0; xx<8; xx++) {
					int c=gdImageGetTrueColorPixel(im, x*8+xx, y*8+yy);
					int r=gdTrueColorGetRed(c);
					int g=gdTrueColorGetGreen(c);
					int b=gdTrueColorGetBlue(c);
					fprintf(cfile, "0x%02X, ", conv_to_8bit(r, g, b));
					bytestotal+=2;
				}
			}
		}
	}
	fprintf(cfile, "\n\t}\n};\n");
	return 1;
err:
	fprintf(stderr, "Error outputing tileset for %s\n", name);
	return 0;
}

static int load_tileset_ext(char *filename, char *designator, char **name);


int load_tileset(xmlNode *tileset, char *designator, char **name) {
	int ret;
	*name=NULL;
	printf("Loading tileset for %s\n", designator);
	if (tileset==NULL) goto err;
	char *twsrc=xmlGetProp(tileset, "source");
	if (twsrc) {
		return load_tileset_ext(twsrc, designator, name);
	}
	char *tws=xmlGetProp(tileset, "tilewidth");
	char *ths=xmlGetProp(tileset, "tileheight");
	char *tnames=xmlGetProp(tileset, "name");
	if (tnames==NULL || tws==NULL || ths==NULL || atoi(tws)!=8 || atoi(ths)!=8) {
		fprintf(stderr, "Tile set %s ignored: need to be 8x8!\n");
		goto err;
	}
	//See if the name is already in the linked list for the tiles
	*name=strdup(tnames);
	conv_to_c_ident(*name);
	for (tmx_llitem_t *t=tmxinfo; t!=NULL; t=t->next) {
		if (strcmp(*name, t->tilename)==0) {
			//we already have this tileset
			return 1;
		}
	}
	char *tcs=xmlGetProp(tileset, "tilecount");
	if (tcs==NULL) {
		fprintf(stderr, "%s: No tilecount attr\n", designator);
		goto err;
	}
	int count=atoi(tcs);
	fprintf(stderr, "Tileset %s: %d tiles\n", tnames, count);
	xmlNode *image=findNodeByName(tileset, "image");
	if (image==NULL) {
		fprintf(stderr, "%s: No image attr\n", designator);
		goto err;
	}
	char *src=xmlGetProp(image, "source");
	if (src==NULL) {
		fprintf(stderr, "%s: No source attr\n", designator);
		goto err;
	}
	char *transcolstr=xmlGetProp(image, "trans");
	int trans_col=-1;
	if (transcolstr) {
		if (transcolstr[0]=='#') transcolstr++;
		int c=strtol(transcolstr, NULL, 16);
		int r=(c>>16)&0xff;
		int g=(c>>8)&0xff;
		int b=(c>>0)&0xff;
		trans_col=conv_to_8bit(r, g, b);
	}
	
	//Output animation frames if any
	uint16_t *animatedTiles=malloc(count*2);
	memset(animatedTiles, 0xff, count*2);
	xmlNodePtr node=tileset->children;
	int animFrCt=0;
	while (node) {
		if (!xmlStrcmp(node->name, "tile")) {
			char *atid=xmlGetProp(node, "id");
			xmlNodePtr anim=findNodeByName(node, "animation");
			if (anim) {
				xmlNodePtr frame=anim->children;
				if (frame) {
					int totalDuration=0;
					if (animFrCt==0) fprintf(cfile, "\nconst tilegfx_anim_frame_t %s_anim_frames[]={\n", *name);
					while(frame) {
						if (!xmlStrcmp(frame->name, "frame")) {
							char *dur=xmlGetProp(frame, "duration");
							if (dur) totalDuration+=atoi(dur);
						}
						frame=frame->next;
					}
					animatedTiles[atoi(atid)]=animFrCt;
					fprintf(cfile, "\t{%d, 0xffff}, ", totalDuration);
					animFrCt++;
					frame=anim->children;
					while(frame) {
						if (!xmlStrcmp(frame->name, "frame")) {
							char *dur=xmlGetProp(frame, "duration");
							char *tid=xmlGetProp(frame, "tileid");
							if (!dur || !tid) break;
							fprintf(cfile, "{%d, %d}, ", atoi(dur), atoi(tid));
							animFrCt++;
						}
						frame=frame->next;
					}
					fprintf(cfile, " //tile %d\n", atoi(atid)-1);
				}
			}
		}
		node=node->next;
	}
	if (animFrCt!=0) {
		fprintf(cfile, "};\n");
		fprintf(cfile, "\nconst uint16_t %s_anim_offsets[]={", *name);
		for (int i=0; i<count; i++) {
			if ((i&7)==0) fprintf(cfile, "\n\t");
			if (animatedTiles[i]==0xffff) {
				fprintf(cfile, "0xffff, ");
			} else {
				fprintf(cfile, "% 6d, ", animatedTiles[i]);
			};
		}
		fprintf(cfile, "};\n");
	}
	free(animatedTiles);

	char *imgfile=malloc(strlen(basedir)+strlen(src)+2);
	sprintf(imgfile, "%s%s", basedir, src);
	FILE *f=fopen(imgfile, "r");
	if (!f) {
		perror(imgfile);
		goto err;
	}
	free(imgfile);
	ret=output_tileset(f, *name, trans_col, animFrCt!=0);
	fclose(f);
	return ret;
err:
	if (*name) free(*name);
	fprintf(stderr, "Couldn't load tileset for %s\n", designator);
	return 0;
}


static int load_tileset_ext(char *filename, char *designator, char **name) {
	int ret;
	xmlDoc *doc=NULL;
	xmlNode *root=NULL;
	fprintf(stderr, "%s: loading external tileset %s\n", designator, filename);
	char *pf=malloc(strlen(filename)+strlen(basedir)+2);
	sprintf(pf, "%s%s", basedir, filename);
	doc=xmlReadFile(pf, NULL, 0);
	if (doc==NULL) {
		fprintf(stderr, "Error opening %s\n", pf);
		goto err;
	}
	root=xmlDocGetRootElement(doc);
	if (strcmp(root->name, "tileset")!=0) {
		fprintf(stderr, "%s: root is no tileset node?\n", pf);
		goto err;
	}
	ret=load_tileset(root, designator, name);
	xmlFreeDoc(doc);
	free(pf);
	return ret;
err:
	printf("Could not load external tile set file %s\n", pf);
	free(pf);
	xmlFreeDoc(doc);
	return 0;
}

int write_map(xmlDoc *doc, xmlNode *layer, char *file, char *designator) {
	char *mn=xmlGetProp(layer, "name");
	char *mh=xmlGetProp(layer, "height");
	char *mw=xmlGetProp(layer, "width");
	if (mn==NULL || mh==NULL || mw==NULL) goto err;

	char *name=malloc(strlen(mn)+strlen(filebase)+2);
	sprintf(name, "%s_%s", filebase, mn);
	conv_to_c_ident(name);
	tmx_llitem_t *i=tmxinfo;
	while (i!=NULL && strcmp(file, i->tmxname)!=0) i=i->next;
	if (i==NULL) goto err;

	int h=atoi(mh);
	int w=atoi(mw);
	fprintf(hfile, "extern const tilegfx_map_t map_%s;\n", name);
	fprintf(cfile, "const tilegfx_map_t map_%s={\n", name);
	fprintf(cfile, "\t.h=%d,\n", h);
	fprintf(cfile, "\t.w=%d,\n", w);
	fprintf(cfile, "\t.gfx=&tileset_%s,\n", i->tilename);
	fprintf(cfile, "\t.tiles={");

	xmlNode *data=findNodeByName(layer, "data");
	if (data==NULL) goto err;
	char *menc=xmlGetProp(data, "encoding");
	if (menc==NULL || strcmp(menc, "csv")!=0) {
		fprintf(stderr, "%s, %s:Unsupported encoding in map layer data; only support csv.\n", designator, name);
		goto err;
	}
	char *csv=xmlNodeListGetString(doc, data->xmlChildrenNode, 1);
	char *p=csv;
	for (int i=0; i<w*h; i++) {
		if ((i&31)==0) fprintf(cfile, "\n\t\t");
		int tile=atoi(p)-1; //csv is base-1 for some reason; we want base-0.
		if (tile==-1) tile=0xffff; //tile not filled in.
		fprintf(cfile, "% 4d,", tile);
		bytestotal+=2;
		p=strchr(p, ',');
		if (p!=NULL && *p==',') {
			p++; 
		} else if (i!=(h*w)-1) {
			printf("%s, %d of %d map entries: csv ends\n", name, i, h*w);
			goto err;
		}
	}
	fprintf(cfile, "\n\t}\n};\n");
	xmlFree(csv);

	free(name);
	return 1;
err:
	fprintf(stderr, "Error parsing layer in %s\n", designator);
	free(name);
	return 0;
}


#define STEP_TILES 0
#define STEP_MAP 1


int parse_tmx_file(char *f, int step) {
	xmlDoc *doc=NULL;
	xmlNode *root=NULL;
	//Find base dir / file base of current file
	char *pathend=strrchr(f, '/');
	if (pathend==NULL) {
		basedir=malloc(1);
		basedir[0]=0;
	} else {
		basedir=strdup(f);
		basedir[pathend-f+1]=0;
	}
	filebase=strdup(f);
	if (pathend!=NULL) {
		strcpy(filebase, pathend+1);
	} else {
		strcpy(filebase, f);
	}
	char *dot=strchr(filebase, '.');
	if (dot!=NULL) *dot=0;
	
	doc=xmlReadFile(f, NULL, 0);
	if (doc==NULL) goto err;
	root=xmlDocGetRootElement(doc);
	if (step==STEP_TILES) {
		//Find tilesets and parse them
		xmlNode *child=root->children;
		while (child!=NULL) {
			if (!xmlStrcmp(child->name, "tileset")) {
				char *name;
				int r=load_tileset(child, f, &name);
				if (!r) goto err;
				tmx_llitem_t *i=malloc(sizeof(tmx_llitem_t));
				i->tmxname=strdup(f);
				i->tilename=name;
				i->next=tmxinfo;
				tmxinfo=i;
			}
			child=child->next;
		}
	} else if (step==STEP_MAP) {
		//Find maps and parse them
		xmlNode *child=root->children;
		while (child!=NULL) {
			if (!xmlStrcmp(child->name, "layer")) {
				int r=write_map(doc, child, f, f);
				if (!r) goto err;
			}
			child=child->next;
		}
	}

	xmlFreeDoc(doc);
	free(basedir);
	free(filebase);
	return 1;
err:
	fprintf(stderr, "Couldn't parse tmx file %s\n", f);
	xmlFreeDoc(doc);
	return 0;
}


int main(int argc, char **argv) {
	bytestotal=0;
	cfile=fopen(argv[1], "w");
	if (cfile==NULL) {
		perror(argv[1]);
		exit(1);
	}
	fprintf(cfile, "//Auto-generated by conv_png_tile\n");
	fprintf(cfile, "#include \"%s\"\n", argv[2]);
	fprintf(cfile, "#define NULL ( (void *) 0)\n");
	
	hfile=fopen(argv[2], "w");
	if (hfile==NULL) {
		perror(argv[2]);
		exit(1);
	}
	fprintf(hfile, "//Auto-generated by conv_png_tile\n");
	fprintf(hfile, "#include \"tilegfx.h\"\n\n");
	for (int i=3; i<argc; i++) {
		int r=parse_tmx_file(argv[i], STEP_TILES);
		if (!r) exit(1);
	}
	for (int i=3; i<argc; i++) {
		int r=parse_tmx_file(argv[i], STEP_MAP);
		if (!r) exit(1);
	}

	xmlCleanupParser();
	printf("Done. Written %d map/gfx bytes (%dK)\n", bytestotal, bytestotal/1024);
	return 0;
}

