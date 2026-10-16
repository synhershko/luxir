# Vector Search

A kNN query is a query like any other. It goes wherever a text query goes:
at the root of a request, under a boolean clause, inside a facet bucket, or
as one source of a hybrid ranking. It takes the same filters, and they apply
*inside* the vector search rather than as a pass over an unfiltered top-k.
Vectors are stored in the index beside the documents, so there is no
separate vector store to run or keep in sync.

## Search by vector

```http
POST /collections/books/_search

{
  "query": {
    "knn": {
      "field":"embedding_v",
      "query":[0.75,0.15,0.10],
      "k":20
    }
  },
  "limit":10,
  "get_scores":true,
  "fields":["id","title_t"]
}
```

```json
{
  "docs": [
    {
      "id": "b1",
      "title_t": "Dune",
      "_score_": 0.99735457
    },
    {
      "id": "b2",
      "title_t": "Dune Messiah",
      "_score_": 0.9652815
    },
    {
      "id": "b3",
      "title_t": "The Making of the Atomic Bomb",
      "_score_": 0.30873275
    }
  ]
}
```

`k` is how many nearest documents the query node produces; `limit` is how
many the request returns. The score is the similarity under the field's
metric. Until you build an approximate index, every search scans the
stored vector column, so a collection is searchable as soon as its
vectors are indexed and the results are exact over the stored vectors
(see [Column encoding](#column-encoding)). [Approximate search](#approximate-search)
is a commit-time option for large collections; the query does not change.

The `books` collection behind every response on this page is defined in
[Define a vector field](#define-a-vector-field) and loaded in
[Index vectors](#index-vectors) below.

## Define a vector field

The `_v` suffix stores a vector, but a searchable field needs a similarity
metric, so define it:

```http
POST /collections/books/_schema

{
  "fields": {
    "embedding_v": {
      "type": "vector",
      "dims": 3,
      "metric": "cosine"
    },
    "passages_vs": {
      "type": "vector",
      "dims": 3,
      "metric": "cosine",
      "multi": true
    }
  }
}
```

Metrics are `l2`, `ip` (inner product), and `cosine`. Set a positive `dims`
to fix the dimension for indexed and query vectors across the collection.
When omitted, the first vector in each segment establishes the dimension.

A multi-valued vector field (`multi: true`, or the `_vs` suffix) holds many
vectors per document, such as one per passage or chunk. Search collapses them
to one hit per document, scored by that document's best vector.

### Truncate Matryoshka embeddings

Some embedding models are trained with
[Matryoshka Representation Learning](https://arxiv.org/abs/2205.13147) (MRL):
the leading components of their output are themselves a usable, lower
dimensional embedding. `truncate_dims` keeps only that prefix:

```http
POST /collections/books/_schema

{
  "fields": {
    "summary_v": {
      "type": "vector",
      "dims": 1024,
      "truncate_dims": 256,
      "metric": "cosine"
    }
  }
}
```

Clients keep sending the full model output, for documents and for kNN
queries. Luxir cuts every vector to its first `truncate_dims` components
before it is stored or searched, so index and query vectors are always
truncated the same way. The column, approximate indexes and scoring all work
on the shorter vector: 256 dimensions here store and scan a quarter of the
bytes of 1024.

- `dims` is the input dimension: when set, every indexed and query vector
  must have exactly `dims` components. `truncate_dims` is the stored and
  searched dimension, and must be positive and less than `dims`.
- Without `dims`, any input with at least `truncate_dims` components is
  accepted. Shorter vectors are rejected (`expected at least
  truncate_dims=256 components, got 128`).
- `cosine` renormalizes the prefix to unit length (a prefix of a unit vector
  is shorter than 1), so stored vectors and scores are those of the
  truncated embedding. `normalized: true` is rejected with `truncate_dims` on
  a cosine field, because the stored vectors are no longer the vectors the
  assertion was about. `normalize_on_write: false` keeps the raw prefix and
  normalizes at query time, as for any cosine field.
- `l2` and `ip` store the prefix as-is. These metrics are defined on raw
  values, magnitude included, and Luxir never rescales vectors for them;
  quietly renormalizing would turn `ip` into cosine. Model providers that
  return unit vectors (OpenAI, Gemini) document renormalizing after
  truncation, and then dot product, cosine and L2 all rank the same. For
  those models, use `cosine` with `truncate_dims` rather than `ip`.
- Reading the field back returns the stored (truncated, and for cosine
  normalized) vector, not the input.
- Changing `truncate_dims` changes the stored dimension, so existing
  segments must be reindexed, as for a `dims` change.

Use it only with models trained for it. Models that document MRL truncation
include OpenAI `text-embedding-3-small` and `-large` (the `dimensions`
parameter), Google `gemini-embedding-001`, Jina `jina-embeddings-v3`, Nomic
`nomic-embed-text-v1.5`, and mixedbread `mxbai-embed-large-v1`. Check the
model card for the dimensions it was trained to support. Truncating an
embedding from a model *not* trained with MRL discards information the model
spread across all of its dimensions, and recall drops sharply; this option
does nothing to compensate.

Some models prescribe extra steps around truncation. Nomic's reference code
applies a layer norm to the full embedding before cutting it; Luxir does not
do that, so send vectors that already have any such pre-truncation step
applied.

### Column encoding

The vector column is the largest part of a vector field on disk and in the
page cache, and every exact search and every approximate-search rescore reads
it. `encoding` chooses how each vector is stored:

| `encoding` | Bytes per vector | Stored value |
|---|---|---|
| `f32` (default) | `4 * dims` | the float32 values |
| `f16` | `2 * dims` | IEEE half precision per element |
| `int8` | `8 + dims` | per-vector scalar quantization: a float offset and scale, plus one byte per dimension |

At 768 dimensions that is 3072, 1536, and 776 bytes per vector.

```json
{
  "fields": {
    "embedding_v": {
      "type": "vector",
      "dims": 768,
      "metric": "cosine",
      "encoding": "int8"
    }
  }
}
```

`int8` quantizes each vector on its own, between its minimum and maximum
element, so a stored vector never depends on other vectors, segments, or
statistics. Merges copy stored vectors unchanged. Cosine vectors are
normalized before they are quantized.

Encoded fields reject vectors with non-finite elements (NaN or infinity), and
`f16` also rejects elements whose magnitude exceeds 65504. A constant vector
is valid under `int8`.

Search scores the query, which stays float32, against the stored encoding:
`exact: true`, the scan of segments without an overlay, and the rescore of
approximate candidates are all exact over the stored vectors, not over the
vectors you submitted. Cosine over `f16` and `int8` divides by the norm of the
stored vector, so scores remain cosines. Vectors read back in `fields` are the
stored values decoded to float32.

Set `keep_original: true` to also keep the submitted float32 vectors in a
separate column. Searches never read it; it costs `4 * dims` bytes per vector
of disk but no page cache in normal operation. Approximate indexes are built
from it when it is present, reading a field back returns the submitted values
(before cosine normalization), and it lets you re-encode the field later from
the exact input.

`encoding` and `keep_original` apply to segments written after the change.
Existing segments keep the encoding they were written with, and a merge of
segments with different encodings writes the most precise of them, so a merge
never loses precision. To re-encode existing data, reindex it.
| `exact` | Require the true top-k over the stored vectors (exact over the field's `encoding`). Scans the column and ignores overlays and the knobs below. |
| `similarity` | Minimum similarity of a hit, in the metric's own space; see [Similarity threshold](#similarity-threshold). |
| `refine_candidates` | Approximate candidates collected before the column rescore. Unset: sized from `k` (see below). Explicit: exactly that many, clamped up to `k`. |
| `ivf.nprobe` | IVF effort, merge-stable (see below). `0` or absent: adaptive, may deepen automatically. |
| `ivf.min_scan_fraction` | A floor on the fraction of live vector values scanned, in `[0,1]`; `0.001` means 0.1%. |

`refine_candidates` sizes the host-side candidate pool, so it means the same
thing whichever index produced the candidates. When unset, the pool is affine
in `k`, with a multiplier that decays from about 10 at `k=1` to a small floor
by `k=1000`: quantization mis-ranks a true neighbor by a roughly constant
number of positions regardless of `k`, so small `k` needs fixed headroom, while
large-`k` requests are recall-oriented retrieval whose near-tied tail does not
benefit from extra overfetch. The final pool is always rescored from the
stored column, sorted by exact score, and collapsed to one hit per
document.

`ivf.nprobe` is interpreted as the number of lists
Luxir would probe if the field were a single IVF index built with
`nlist = sqrt(live_vector_count)`, capped the same way as the builder's
`nlist`. That total effort is spread across the current per-segment indexes,
so a merge does not change the effort a request asks for. An explicit `nprobe`
pins the effort cap; an adaptive one may deepen when filters or collapse
underfill the result. Probed lists are always scanned in full; `nprobe` is the
only work limiter.

### Scoring

L2 scores are `1 / (1 + squared_distance)`. Inner product scores are the dot
product. Cosine is inner product over normalized vectors. The score
distribution depends on the embedding model and corpus, so calibrate
thresholds per deployment.

### Similarity threshold

`similarity` drops the nearest neighbors that are not similar enough, with the
semantics of Elasticsearch's `knn.similarity`. The value is in the metric's
space, not the score's:

| Metric | `similarity` means | Hit kept when |
|---|---|---|
| `l2` | Maximum L2 distance (not squared); must be `>= 0` | `distance <= similarity` |
| `ip` | Minimum dot product | `dot >= similarity` |
| `cosine` | Minimum cosine similarity | `cosine >= similarity` |

```json
{"query": {"knn": {"field": "embedding_v", "query": [0.75, 0.15, 0.10], "k": 20, "similarity": 0.8}}}
```

The threshold trims the `k` nearest documents after the full-precision
rescore; it does not search past them, so a request can match fewer than `k`
documents. The comparison uses each document's best vector, before any
enclosing `boost`. Trimmed documents are not matches: they leave `found`,
facets, and metrics as well as the returned list, on exact and approximate
segments and in parallel execution alike.

Cosine fields normalize on write by default (`normalize_on_write: true`), so
reading the vector column back returns the unit vector rather than the bytes
you submitted. Vector fields are returned only when named in `fields`:

```http
POST /collections/books/_search

{"query": "id:b2", "fields": ["id", "embedding_v"]}
```

```json
{
  "docs": [
    {
      "id": "b2",
      "embedding_v": [0.8846517, 0.44232586, 0.14744195]
    }
  ]
}
```

That is `[0.6, 0.3, 0.1]` at unit length. Set `normalize_on_write: false` to
keep the raw vectors in the column; candidate rescoring then normalizes at
query time. Zero and near-zero cosine vectors have no direction and are
skipped at index time.

### Measure recall

IVF+PQ is approximate. Higher `ivf.nprobe` and higher `refine_candidates`
raise recall and latency. Setting `nprobe` to at least the reference
`sqrt(live_vector_count)`, or `ivf.min_scan_fraction` to `1`, makes the search
exhaustive over IVF lists, but PQ ordering still decides which candidates are
rescored.

`exact: true` uses a path that guarantees the true top-k over the stored
vectors and does not consult overlays. Everything else about
the query is identical (filters, multi-valued collapse, score scale), which
makes it the built-in ground truth for measuring your ANN settings. Run the
same query twice, once with `exact: true`, and compare the two lists. With
an `f16` or `int8` encoding this measures the approximate index against the
encoding; to measure the encoding itself, compare against the same vectors
in an `f32` field.

Results are deterministic. A query run with intra-request parallelism returns
bit-identical results to a serial run of the same query.

## Limits

- IVF+PQ is the only approximate index type. There is no HNSW yet.
- Filters are always applied inside the vector search. There is no cost-based
  choice between filtered approximate search and exact search over the
  filtered set, and no automatic exact fallback when a selective filter still
  starves the approximate search after breadth deepening. Use `exact: true`
  when exact filtered top-k is the required contract.
- There is no public command to remove or rebuild an overlay that a segment
  already has. New and merged segments build from their columns, and a segment
  without an overlay can receive one through a later selected commit.
- Skipped zero-norm cosine vectors are reported only as a server-log warning,
  not in the update response.
- Vector fields are returned only when named in `fields`; they are excluded
  from default projection.
- Changing `encoding` does not re-encode existing segments; reindex to apply
  it to existing documents.
