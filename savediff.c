/* What a save actually contains, aside from addresses - and whether two of them
 * are the same save wearing different addresses.
 *
 * The cross-session wall is that the game's private allocations land at different
 * addresses in a fresh process, so a byte snapshot restored into the new layout
 * aims half its pointers into holes. The open question is whether the SAVE ITSELF
 * is deterministic - same regions, same sizes, same non-pointer content - with
 * only the addresses shifted around it. If it is, pinning those allocations to
 * fixed bases makes the snapshot restore cleanly; if the content differs run to
 * run, pinning cannot help and the whole approach changes.
 *
 * This reads the slotfiles the engine writes under D3D9SW_SLOTFILE=1 - the .regions
 * text index and the .bin bytes beside it - and never touches the game. It runs
 * offline, after the fact, on files two different sessions produced.
 *
 * strip <slotN.regions>            profile one save: per region how much is
 *                                  heap pointers, module/code pointers, zero, and
 *                                  plain data, plus a content hash that ignores
 *                                  every address.
 * diff  <A.regions> <B.regions>    match two saves' regions by size and by that
 *                                  address-blind hash, and report which are the
 *                                  same content at a shifted address, which differ,
 *                                  and which exist in only one - the direct read of
 *                                  "deterministic but moved" versus "actually changed".
 *
 * A word is called a pointer when it lands inside a captured region (heap->heap)
 * or inside a module's mapped range (code, vtables, statics). Those are exactly
 * the words that move between sessions, so the address-blind hash replaces each
 * with a class token and keeps only the data. Two regions whose data hashes match
 * are the same content; a consistent base delta across the matched set means the
 * whole allocation merely slid, which is the friendly case for pinning. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { uint32_t base, size, off, prot; } Region;
typedef struct { uint32_t lo, hi; char name[64]; } Mod;

typedef struct {
	char name[512];
	Region *regs;
	int nregs;
	Mod *mods;
	int nmods;
	uint8_t *bin;
	size_t binlen;
	uint32_t *blo, *bhi; /* bands: held heaps, stacks, TEBs, PEB */
	int nbands;
	/* sorted range tables for O(log n) membership */
	uint32_t *rlo, *rhi;
	int nr;
	uint32_t *mlo, *mhi;
	int nm;
	uint32_t *bslo, *bshi; /* sorted bands */
	int nb;
} Save;

static int cmp_u32(const void *a, const void *b)
{
	uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
	return x < y ? -1 : x > y ? 1 : 0;
}

typedef struct { uint32_t lo, hi; } Iv;

static int cmp_iv(const void *a, const void *b)
{
	uint32_t x = ((const Iv *)a)->lo, y = ((const Iv *)b)->lo;
	return x < y ? -1 : x > y ? 1 : 0;
}

/* Is v inside any [lo[i], hi[i])? lo is sorted; hi[i] belongs to lo[i]. Because
 * the ranges do not overlap (distinct allocations, distinct modules), the last
 * lo <= v is the only candidate. */
static int in_ranges(uint32_t v, const uint32_t *lo, const uint32_t *hi, int n)
{
	int a = 0, b = n - 1, best = -1;

	while (a <= b) {
		int m = (a + b) / 2;

		if (lo[m] <= v) {
			best = m;
			a = m + 1;
		} else {
			b = m - 1;
		}
	}
	return best >= 0 && v < hi[best];
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	buf = (char *)malloc((size_t)n + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n] = 0;
	*len = (size_t)n;
	fclose(f);
	return buf;
}

/* Load a save from its .regions path; the .bin is the same name with the
 * extension swapped. */
static int load_save(Save *s, const char *regions_path)
{
	char bin_path[512];
	char *txt;
	size_t txtlen;
	char *line, *save_ptr = NULL;
	const char *dot;
	int cap_r = 16, cap_m = 8, cap_b = 16;

	memset(s, 0, sizeof(*s));
	strncpy(s->name, regions_path, sizeof(s->name) - 1);

	txt = slurp(regions_path, &txtlen);
	if (!txt) {
		fprintf(stderr, "cannot read %s\n", regions_path);
		return 0;
	}
	s->regs = (Region *)malloc((size_t)cap_r * sizeof(Region));
	s->mods = (Mod *)malloc((size_t)cap_m * sizeof(Mod));
	s->blo = (uint32_t *)malloc((size_t)cap_b * sizeof(uint32_t));
	s->bhi = (uint32_t *)malloc((size_t)cap_b * sizeof(uint32_t));

	for (line = strtok_r(txt, "\r\n", &save_ptr); line;
	     line = strtok_r(NULL, "\r\n", &save_ptr)) {
		if (line[0] == '#') {
			uint32_t lo, hi;
			char nm[64];

			if (sscanf(line, "# module %x %x %63s", &lo, &hi, nm) == 3) {
				if (s->nmods >= cap_m) {
					cap_m *= 2;
					s->mods = (Mod *)realloc(s->mods,
								 (size_t)cap_m * sizeof(Mod));
				}
				s->mods[s->nmods].lo = lo;
				s->mods[s->nmods].hi = hi;
				strncpy(s->mods[s->nmods].name, nm,
					sizeof(s->mods[s->nmods].name) - 1);
				s->mods[s->nmods].name[sizeof(s->mods[s->nmods].name) - 1] = 0;
				s->nmods++;
			} else if (sscanf(line, "# band %x %x", &lo, &hi) == 2 && hi > lo) {
				if (s->nbands >= cap_b) {
					cap_b *= 2;
					s->blo = (uint32_t *)realloc(s->blo,
								     (size_t)cap_b * sizeof(uint32_t));
					s->bhi = (uint32_t *)realloc(s->bhi,
								     (size_t)cap_b * sizeof(uint32_t));
				}
				s->blo[s->nbands] = lo;
				s->bhi[s->nbands] = hi;
				s->nbands++;
			}
			continue; /* other # lines are headers/player info */
		}
		{
			uint32_t base, size, off, prot;

			if (sscanf(line, "%x %x %x %x", &base, &size, &off, &prot) == 4) {
				if (s->nregs >= cap_r) {
					cap_r *= 2;
					s->regs = (Region *)realloc(s->regs,
								    (size_t)cap_r * sizeof(Region));
				}
				s->regs[s->nregs].base = base;
				s->regs[s->nregs].size = size;
				s->regs[s->nregs].off = off;
				s->regs[s->nregs].prot = prot;
				s->nregs++;
			}
		}
	}
	free(txt);

	strncpy(bin_path, regions_path, sizeof(bin_path) - 1);
	bin_path[sizeof(bin_path) - 1] = 0;
	dot = strrchr(bin_path, '.');
	if (dot)
		strcpy(bin_path + (dot - bin_path), ".bin");
	else
		strncat(bin_path, ".bin", sizeof(bin_path) - strlen(bin_path) - 1);
	s->bin = (uint8_t *)slurp(bin_path, &s->binlen);
	if (!s->bin) {
		fprintf(stderr, "cannot read %s\n", bin_path);
		return 0;
	}

	/* Build the sorted range tables once. */
	s->rlo = (uint32_t *)malloc((size_t)s->nregs * sizeof(uint32_t));
	s->rhi = (uint32_t *)malloc((size_t)s->nregs * sizeof(uint32_t));
	{
		int i;
		uint32_t *tmp = (uint32_t *)malloc((size_t)s->nregs * sizeof(uint32_t));

		for (i = 0; i < s->nregs; i++)
			tmp[i] = s->regs[i].base;
		memcpy(s->rlo, tmp, (size_t)s->nregs * sizeof(uint32_t));
		qsort(s->rlo, (size_t)s->nregs, sizeof(uint32_t), cmp_u32);
		/* hi paired to the sorted lo: find each region by base */
		for (i = 0; i < s->nregs; i++) {
			int j;
			for (j = 0; j < s->nregs; j++)
				if (s->regs[j].base == s->rlo[i]) {
					s->rhi[i] = s->regs[j].base + s->regs[j].size;
					break;
				}
		}
		free(tmp);
		s->nr = s->nregs;
	}
	s->mlo = (uint32_t *)malloc((size_t)(s->nmods + 1) * sizeof(uint32_t));
	s->mhi = (uint32_t *)malloc((size_t)(s->nmods + 1) * sizeof(uint32_t));
	{
		int i, j;
		for (i = 0; i < s->nmods; i++)
			s->mlo[i] = s->mods[i].lo;
		qsort(s->mlo, (size_t)s->nmods, sizeof(uint32_t), cmp_u32);
		for (i = 0; i < s->nmods; i++)
			for (j = 0; j < s->nmods; j++)
				if (s->mods[j].lo == s->mlo[i]) {
					s->mhi[i] = s->mods[j].hi;
					break;
				}
		s->nm = s->nmods;
	}
	/* Bands may overlap (a stack inside a heap), so sort and merge into
	 * non-overlapping intervals for the membership test. */
	if (s->nbands) {
		Iv *iv = (Iv *)malloc((size_t)s->nbands * sizeof(Iv));
		int i, m = 0;

		for (i = 0; i < s->nbands; i++) {
			iv[i].lo = s->blo[i];
			iv[i].hi = s->bhi[i];
		}
		qsort(iv, (size_t)s->nbands, sizeof(Iv), cmp_iv);
		s->bslo = (uint32_t *)malloc((size_t)s->nbands * sizeof(uint32_t));
		s->bshi = (uint32_t *)malloc((size_t)s->nbands * sizeof(uint32_t));
		for (i = 0; i < s->nbands; i++) {
			if (m > 0 && iv[i].lo <= s->bshi[m - 1]) {
				if (iv[i].hi > s->bshi[m - 1])
					s->bshi[m - 1] = iv[i].hi;
			} else {
				s->bslo[m] = iv[i].lo;
				s->bshi[m] = iv[i].hi;
				m++;
			}
		}
		s->nb = m;
		free(iv);
	}
	return 1;
}

/* Per-region tally plus an address-blind content hash. */
typedef struct {
	uint64_t hash;	 /* FNV-1a over class-folded words */
	uint32_t rptr;	 /* words that point into a captured region */
	uint32_t mptr;	 /* words that point into a module image */
	uint32_t bptr;	 /* words that point into a held heap, stack or TEB */
	uint32_t zero;	 /* zero words */
	uint32_t data;	 /* everything else - plain data */
	uint32_t words;
} Profile;

/* Fold a word to a class token that ignores the address it may be: a pointer
 * into a captured region or a module image becomes a fixed token, so two saves
 * whose only difference is where things landed fold identically. Plain data and
 * zero fold to themselves, so a genuine content change still shows. */
static uint32_t masked_fold(const Save *s, uint32_t v)
{
	if (v == 0)
		return 0;
	if (in_ranges(v, s->rlo, s->rhi, s->nr))
		return 0xE1E1E1E1u; /* pointer into a captured region */
	if (in_ranges(v, s->mlo, s->mhi, s->nm))
		return 0xE2E2E2E2u; /* pointer into a module image */
	if (in_ranges(v, s->bslo, s->bshi, s->nb))
		return 0xE3E3E3E3u; /* pointer into a held heap, stack or TEB */
	return v;		    /* real content */
}

static void profile_region(const Save *s, const Region *r, Profile *p)
{
	uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
	const uint32_t *w;
	uint32_t i, n;

	memset(p, 0, sizeof(*p));
	if ((size_t)r->off + r->size > s->binlen)
		return; /* truncated .bin; leave zeroed */
	w = (const uint32_t *)(s->bin + r->off);
	n = r->size / 4;
	p->words = n;
	for (i = 0; i < n; i++) {
		uint32_t v = w[i], tok = masked_fold(s, v);

		if (v == 0)
			p->zero++;
		else if (tok == 0xE1E1E1E1u)
			p->rptr++;
		else if (tok == 0xE2E2E2E2u)
			p->mptr++;
		else if (tok == 0xE3E3E3E3u)
			p->bptr++;
		else
			p->data++;
		h ^= tok;
		h *= 1099511628211ULL;
	}
	p->hash = h;
}

/* Words that differ between two equal-size regions AFTER masking addresses -
 * genuine content divergence, not "it moved." A position where both fold to the
 * same token (both zero, both heap pointers, both the same data) does not count;
 * a data word that changed, or data-vs-pointer, does. */
static uint32_t masked_word_diff(const Save *sa, const Region *ra,
				 const Save *sb, const Region *rb)
{
	const uint32_t *wa, *wb;
	uint32_t i, n, diff = 0;

	if ((size_t)ra->off + ra->size > sa->binlen ||
	    (size_t)rb->off + rb->size > sb->binlen || ra->size != rb->size)
		return 0xFFFFFFFFu; /* cannot compare */
	wa = (const uint32_t *)(sa->bin + ra->off);
	wb = (const uint32_t *)(sb->bin + rb->off);
	n = ra->size / 4;
	for (i = 0; i < n; i++)
		if (masked_fold(sa, wa[i]) != masked_fold(sb, wb[i]))
			diff++;
	return diff;
}

static double mb(uint64_t b) { return (double)b / (1024.0 * 1024.0); }

static void do_strip(const char *path)
{
	Save s;
	int i;
	uint64_t t_words = 0, t_rptr = 0, t_mptr = 0, t_bptr = 0, t_zero = 0, t_data = 0;
	uint64_t fp = 1469598103934665603ULL;

	if (!load_save(&s, path))
		return;
	printf("save %s: %d region(s), %d module(s), %.1f MB of bytes\n\n",
	       path, s.nregs, s.nmods, mb(s.binlen));
	printf("  %-10s %9s  %5s %5s %5s %5s %5s   %-16s\n", "base", "size", "%rptr",
	       "%mod", "%band", "%zero", "%data", "content-hash");
	for (i = 0; i < s.nregs; i++) {
		Profile p;
		double d = s.regs[i].size ? 100.0 / (double)(s.regs[i].size / 4) : 0.0;

		profile_region(&s, &s.regs[i], &p);
		printf("  %08X %9u  %4.0f%% %4.0f%% %4.0f%% %4.0f%% %4.0f%%   %016llX\n",
		       s.regs[i].base, s.regs[i].size, p.rptr * d, p.mptr * d, p.bptr * d,
		       p.zero * d, p.data * d, (unsigned long long)p.hash);
		t_words += p.words;
		t_rptr += p.rptr;
		t_mptr += p.mptr;
		t_bptr += p.bptr;
		t_zero += p.zero;
		t_data += p.data;
		/* the save's own address-blind fingerprint: fold (size, hash) pairs */
		fp ^= (uint64_t)s.regs[i].size;
		fp *= 1099511628211ULL;
		fp ^= p.hash;
		fp *= 1099511628211ULL;
	}
	if (t_words == 0)
		t_words = 1;
	printf("\n  totals over %llu words: region-ptr %.1f%%, module-ptr %.1f%%, "
	       "band-ptr %.1f%%, zero %.1f%%, data %.1f%%\n",
	       (unsigned long long)t_words, 100.0 * (double)t_rptr / (double)t_words,
	       100.0 * (double)t_mptr / (double)t_words,
	       100.0 * (double)t_bptr / (double)t_words,
	       100.0 * (double)t_zero / (double)t_words,
	       100.0 * (double)t_data / (double)t_words);
	printf("  save fingerprint (address-blind): %016llX\n",
	       (unsigned long long)fp);
	printf("  -> pointers are %.1f%% of the save; the rest is data that should be "
	       "identical run to run if the game is deterministic\n",
	       100.0 * (double)(t_rptr + t_mptr + t_bptr) / (double)t_words);
}

/* For matching, a region reduced to (size, address-blind hash). */
typedef struct {
	uint32_t size, base;
	uint64_t hash;
	int used;
} Key;

static void do_diff(const char *pa, const char *pb)
{
	Save A, B;
	Key *ka, *kb;
	int i, j;
	int matched = 0, differ = 0, only_a = 0, only_b = 0;
	uint64_t bytes_matched = 0, bytes_total_a = 0;
	long long *deltas;
	int ndelta = 0;
	/* Content-divergence accounting for the same-size "differs" pairs. */
	uint64_t diff_words = 0, data_words = 0;
	int hist[6] = { 0 }; /* ==0, <=0.1%, <=1%, <=10%, <=50%, >50% of words */
	struct { uint32_t base, size, nd, words; } top[12];
	int ntop = 0, k;

	if (!load_save(&A, pa) || !load_save(&B, pb))
		return;

	ka = (Key *)calloc((size_t)A.nregs, sizeof(Key));
	kb = (Key *)calloc((size_t)B.nregs, sizeof(Key));
	deltas = (long long *)malloc((size_t)A.nregs * sizeof(long long));

	for (i = 0; i < A.nregs; i++) {
		Profile p;
		profile_region(&A, &A.regs[i], &p);
		ka[i].size = A.regs[i].size;
		ka[i].base = A.regs[i].base;
		ka[i].hash = p.hash;
		bytes_total_a += A.regs[i].size;
	}
	for (j = 0; j < B.nregs; j++) {
		Profile p;
		profile_region(&B, &B.regs[j], &p);
		kb[j].size = B.regs[j].size;
		kb[j].base = B.regs[j].base;
		kb[j].hash = p.hash;
	}

	printf("diff\n  A %s: %d region(s), %.1f MB\n  B %s: %d region(s), %.1f MB\n\n",
	       pa, A.nregs, mb(bytes_total_a), pb, B.nregs, mb(B.binlen));

	/* Reliable pairing first: a region at the SAME base and size in both saves
	 * is unambiguously the same allocation, so its masked divergence is honest.
	 * Regions that moved cannot be paired offline without guessing, and a wrong
	 * guess between two same-size strangers reads as ~100% difference - noise,
	 * not divergence. This is the number to trust; the content-matched section
	 * below is best-effort for the moved remainder. */
	{
		uint64_t sb_words = 0, sb_diff = 0;
		int sb_n = 0, sh[6] = { 0 };

		for (i = 0; i < A.nregs; i++)
			for (j = 0; j < B.nregs; j++)
				if (B.regs[j].base == A.regs[i].base &&
				    B.regs[j].size == A.regs[i].size) {
					uint32_t nd = masked_word_diff(&A, &A.regs[i], &B,
								       &B.regs[j]);
					uint32_t words = A.regs[i].size / 4;
					double frac = words ? (double)nd / (double)words : 0.0;

					sb_n++;
					sb_words += words;
					sb_diff += nd;
					if (nd == 0)
						sh[0]++;
					else if (frac <= 0.001)
						sh[1]++;
					else if (frac <= 0.01)
						sh[2]++;
					else if (frac <= 0.10)
						sh[3]++;
					else if (frac <= 0.50)
						sh[4]++;
					else
						sh[5]++;
					break;
				}
		printf("  SAME-ADDRESS regions (%d, reliable - same base+size both runs):\n",
		       sb_n);
		printf("      %llu of %llu data words differ (%.4f%%) after masking every "
		       "pointer class\n",
		       (unsigned long long)sb_diff, (unsigned long long)sb_words,
		       sb_words ? 100.0 * (double)sb_diff / (double)sb_words : 0.0);
		printf("      identical %d | <=0.1%% %d | <=1%% %d | <=10%% %d | <=50%% %d | "
		       ">50%% %d\n",
		       sh[0], sh[1], sh[2], sh[3], sh[4], sh[5]);
		printf("  (this is the trustworthy determinism reading; the moved-region "
		       "figures below are best-effort and inflated by same-size mispairs)\n\n");
	}

	/* Exact match on (size, address-blind hash): same content at a moved base. */
	for (i = 0; i < A.nregs; i++) {
		for (j = 0; j < B.nregs; j++) {
			if (!kb[j].used && kb[j].size == ka[i].size &&
			    kb[j].hash == ka[i].hash) {
				kb[j].used = 1;
				ka[i].used = 1;
				matched++;
				bytes_matched += ka[i].size;
				deltas[ndelta++] =
					(long long)kb[j].base - (long long)ka[i].base;
				break;
			}
		}
	}
	/* Among the leftovers, pair each A region with the same-size B region it
	 * differs from LEAST (after masking addresses), so the count that comes out
	 * is genuine content divergence and not an accident of which same-size
	 * region we happened to grab first. */
	for (i = 0; i < A.nregs; i++) {
		int best = -1;
		uint32_t bestnd = 0xFFFFFFFFu, words;
		double frac;

		if (ka[i].used)
			continue;
		for (j = 0; j < B.nregs; j++) {
			uint32_t nd;

			if (kb[j].used || kb[j].size != ka[i].size)
				continue;
			nd = masked_word_diff(&A, &A.regs[i], &B, &B.regs[j]);
			if (nd < bestnd) {
				bestnd = nd;
				best = j;
			}
		}
		if (best < 0)
			continue; /* no same-size partner - stays "only in A" */
		kb[best].used = 1;
		ka[i].used = 1;
		differ++;
		words = ka[i].size / 4;
		data_words += words;
		diff_words += bestnd;
		frac = words ? (double)bestnd / (double)words : 0.0;
		if (bestnd == 0)
			hist[0]++;
		else if (frac <= 0.001)
			hist[1]++;
		else if (frac <= 0.01)
			hist[2]++;
		else if (frac <= 0.10)
			hist[3]++;
		else if (frac <= 0.50)
			hist[4]++;
		else
			hist[5]++;
		/* Keep the dozen biggest absolute divergences, insertion-sorted. */
		if (ntop < 12 || bestnd > top[ntop - 1].nd) {
			int at = ntop < 12 ? ntop : 11;
			while (at > 0 && top[at - 1].nd < bestnd) {
				if (at < 12)
					top[at] = top[at - 1];
				at--;
			}
			top[at].base = ka[i].base;
			top[at].size = ka[i].size;
			top[at].nd = bestnd;
			top[at].words = words;
			if (ntop < 12)
				ntop++;
		}
	}
	for (i = 0; i < A.nregs; i++)
		if (!ka[i].used)
			only_a++;
	for (j = 0; j < B.nregs; j++)
		if (!kb[j].used)
			only_b++;

	printf("  matched (same content, moved base): %d region(s), %.1f MB "
	       "(%.1f%% of A by bytes)\n",
	       matched, mb(bytes_matched),
	       bytes_total_a ? 100.0 * (double)bytes_matched / (double)bytes_total_a : 0.0);
	printf("  same size, content DIFFERS:         %d region(s)\n", differ);
	printf("  only in A:                          %d region(s)\n", only_a);
	printf("  only in B:                          %d region(s)\n", only_b);

	/* The point of the whole exercise: of the regions that differ, how much
	 * actually differs once addresses are masked out. A region that changed in a
	 * handful of words is live values - position, a timer, an enemy coordinate -
	 * the kind a same-session restore already rewrites wholesale. A region that
	 * changed across a large fraction of its words is structural divergence, the
	 * thing that would actually break a restore. */
	if (differ) {
		uint64_t total_words = bytes_matched / 4 + data_words;

		printf("\n  content divergence in the %d differing region(s), by how much of "
		       "each differs (addresses masked out):\n", differ);
		printf("      identical after masking (moved only): %d\n", hist[0]);
		printf("      <=0.1%% of words differ:               %d   <- live values, trivially rewritable\n",
		       hist[1]);
		printf("      <=1%%   of words differ:               %d\n", hist[2]);
		printf("      <=10%%  of words differ:               %d\n", hist[3]);
		printf("      <=50%%  of words differ:               %d\n", hist[4]);
		printf("      >50%%   of words differ:               %d   <- structural, the real problem\n",
		       hist[5]);
		printf("  overall: %llu of %llu data words differ (%.3f%%) across matched+differing "
		       "regions - the rest is byte-identical or address-only\n",
		       (unsigned long long)diff_words, (unsigned long long)total_words,
		       total_words ? 100.0 * (double)diff_words / (double)total_words : 0.0);
		if (ntop) {
			printf("  biggest divergences (region: differing/total words):\n");
			for (k = 0; k < ntop; k++)
				printf("      %08X  %8u words  %8u differ (%.1f%%)\n",
				       top[k].base, top[k].words, top[k].nd,
				       top[k].words ? 100.0 * (double)top[k].nd /
							      (double)top[k].words
						     : 0.0);
		}
	}

	/* Delta pattern: one consistent shift is the friendly case for pinning; a
	 * spread means every allocation moved on its own. */
	if (ndelta) {
		long long mn = deltas[0], mx = deltas[0];
		int same0 = 0;

		for (i = 0; i < ndelta; i++) {
			if (deltas[i] < mn)
				mn = deltas[i];
			if (deltas[i] > mx)
				mx = deltas[i];
			if (deltas[i] == 0)
				same0++;
		}
		printf("\n  base deltas over %d matched: min %lld, max %lld, %d at zero "
		       "(same address both runs)\n",
		       ndelta, mn, mx, same0);
		if (mn == mx)
			printf("  -> every matched region moved by the SAME delta: the whole "
			       "layout slid as one block. Pinning that block reproduces it.\n");
		else
			printf("  -> matched regions moved by DIFFERENT deltas: allocations are "
			       "placed independently. Each needs its own pinned base.\n");
	}
	printf("\n  read: matched %% is the save's deterministic core - same content, "
	       "only the address moved. 'content DIFFERS' is genuine run-to-run "
	       "variation; 'only in' is a region one run had and the other did not.\n");
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "strip")) {
		do_strip(argv[2]);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "diff")) {
		do_diff(argv[2], argv[3]);
		return 0;
	}
	fprintf(stderr,
		"usage:\n"
		"  savediff strip <slotN.regions>            profile one save aside from addresses\n"
		"  savediff diff  <A.regions> <B.regions>    compare two sessions' saves\n"
		"\nProduce the files with D3D9SW_SLOTFILE=1; copy a session's\n"
		"d3d9sw_slotN.{regions,bin} aside before the next session overwrites them.\n");
	return 2;
}
