#include "../src/meshoptimizer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "objparser.h"

const size_t kCacheSize = 16;

struct Vertex
{
	float px, py, pz;
	float nx, ny, nz;
	float tx, ty;
};

struct Mesh
{
	std::vector<Vertex> vertices;
	std::vector<unsigned int> indices;
};

Mesh generatePlane(unsigned int N)
{
	Mesh result;

	result.vertices.reserve((N + 1) * (N + 1));
	result.indices.reserve(N * N * 6);

	for (unsigned int y = 0; y <= N; ++y)
		for (unsigned int x = 0; x <= N; ++x)
		{
			Vertex v = {float(x), float(y), 0, 0, 0, 1, float(x) / float(N), float(y) / float(N)};

			result.vertices.push_back(v);
		}

	for (unsigned int y = 0; y < N; ++y)
		for (unsigned int x = 0; x < N; ++x)
		{
			result.indices.push_back((y + 0) * (N + 1) + (x + 0));
			result.indices.push_back((y + 0) * (N + 1) + (x + 1));
			result.indices.push_back((y + 1) * (N + 1) + (x + 0));

			result.indices.push_back((y + 1) * (N + 1) + (x + 0));
			result.indices.push_back((y + 0) * (N + 1) + (x + 1));
			result.indices.push_back((y + 1) * (N + 1) + (x + 1));
		}

	return result;
}

Mesh parseObj(const char* path, clock_t& reindex)
{
	ObjFile file;

	if (!objParseFile(file, path))
	{
		printf("Error loading %s: file not found\n", path);
		return Mesh();
	}

	if (!objValidate(file))
	{
		printf("Error loading %s: invalid file data\n", path);
		return Mesh();
	}

	objTriangulate(file);

	size_t total_indices = file.f.size() / 3;

	std::vector<Vertex> vertices;
	vertices.reserve(total_indices);

	for (size_t i = 0; i < file.f.size(); i += 3)
	{
		int vi = file.f[i + 0];
		int vti = file.f[i + 1];
		int vni = file.f[i + 2];

		Vertex v =
		    {
		        file.v[vi * 3 + 0],
		        file.v[vi * 3 + 1],
		        file.v[vi * 3 + 2],

		        vni >= 0 ? file.vn[vni * 3 + 0] : 0,
		        vni >= 0 ? file.vn[vni * 3 + 1] : 0,
		        vni >= 0 ? file.vn[vni * 3 + 2] : 0,

		        vti >= 0 ? file.vt[vti * 3 + 0] : 0,
		        vti >= 0 ? file.vt[vti * 3 + 1] : 0,
		    };

		vertices.push_back(v);
	}

	reindex = clock();

	Mesh result;

	std::vector<unsigned int> remap(total_indices);

	size_t total_vertices = meshopt_generateVertexRemap(&remap[0], NULL, total_indices, &vertices[0], total_indices, sizeof(Vertex));

	result.indices.resize(total_indices);
	meshopt_remapIndexBuffer(&result.indices[0], NULL, total_indices, &remap[0]);

	result.vertices.resize(total_vertices);
	meshopt_remapVertexBuffer(&result.vertices[0], &vertices[0], total_indices, sizeof(Vertex), &remap[0]);

	return result;
}

void optNone(Mesh& mesh)
{
	(void)mesh;
}

void optRandomShuffle(Mesh& mesh)
{
	std::vector<unsigned int> faces(mesh.indices.size() / 3);

	for (size_t i = 0; i < faces.size(); ++i)
		faces[i] = static_cast<unsigned int>(i);

	std::random_shuffle(faces.begin(), faces.end());

	std::vector<unsigned int> result(mesh.indices.size());

	for (size_t i = 0; i < faces.size(); ++i)
	{
		result[i * 3 + 0] = mesh.indices[faces[i] * 3 + 0];
		result[i * 3 + 1] = mesh.indices[faces[i] * 3 + 1];
		result[i * 3 + 2] = mesh.indices[faces[i] * 3 + 2];
	}

	mesh.indices.swap(result);
}

void optCache(Mesh& mesh)
{
	meshopt_optimizeVertexCache(&mesh.indices[0], &mesh.indices[0], mesh.indices.size(), mesh.vertices.size());
}

void optCacheFifo(Mesh& mesh)
{
	meshopt_optimizeVertexCacheFifo(&mesh.indices[0], &mesh.indices[0], mesh.indices.size(), mesh.vertices.size(), kCacheSize);
}

void optOverdraw(Mesh& mesh)
{
	// use worst-case ACMR threshold so that overdraw optimizer can sort *all* triangles
	// warning: this significantly deteriorates the vertex cache efficiency so it is not advised; look at optComplete for the recommended method
	const float kThreshold = 3.f;
	meshopt_optimizeOverdraw(&mesh.indices[0], &mesh.indices[0], mesh.indices.size(), &mesh.vertices[0].px, mesh.vertices.size(), sizeof(Vertex), kThreshold);
}

void optFetch(Mesh& mesh)
{
	meshopt_optimizeVertexFetch(&mesh.vertices[0], &mesh.indices[0], mesh.indices.size(), &mesh.vertices[0], mesh.vertices.size(), sizeof(Vertex));
}

void optComplete(Mesh& mesh)
{
	// vertex cache optimization should go first as it provides data for overdraw
	meshopt_optimizeVertexCache(&mesh.indices[0], &mesh.indices[0], mesh.indices.size(), mesh.vertices.size());

	// reorder indices for overdraw, balancing overdraw and vertex cache efficiency
	const float kThreshold = 1.05f; // allow up to 5% worse ACMR to get more reordering opportunities for overdraw
	meshopt_optimizeOverdraw(&mesh.indices[0], &mesh.indices[0], mesh.indices.size(), &mesh.vertices[0].px, mesh.vertices.size(), sizeof(Vertex), kThreshold);

	// vertex fetch optimization should go last as it depends on the final index order
	meshopt_optimizeVertexFetch(&mesh.vertices[0], &mesh.indices[0], mesh.indices.size(), &mesh.vertices[0], mesh.vertices.size(), sizeof(Vertex));
}

void optCompleteSimplify(Mesh& mesh)
{
	static const size_t lod_count = 5;

	// generate 4 LOD levels (1-4), with each subsequent LOD using 70% triangles
	// note that each LOD uses the same (shared) vertex buffer
	// vertex cache optimization should go first as it provides data for overdraw
	std::vector<unsigned int> lods[lod_count];

	lods[0] = mesh.indices;

	for (size_t i = 1; i < lod_count; ++i)
	{
		std::vector<unsigned int>& lod = lods[i];

		float threshold = powf(0.7f, float(i));
		size_t target_index_count = size_t(mesh.indices.size() * threshold);

		// we can simplify all the way from base level or from the last result
		// simplifying from the base level sometimes produces better results, but simplifying from last level is faster
		const std::vector<unsigned int>& source = lods[i - 1];

		lod.resize(source.size());
		lod.resize(meshopt_simplify(&lod[0], &source[0], source.size(), &mesh.vertices[0].px, mesh.vertices.size(), sizeof(Vertex), target_index_count));
	}

	// optimize each individual LOD for vertex cache & overdraw
	for (size_t i = 0; i < lod_count; ++i)
	{
		std::vector<unsigned int>& lod = lods[i];

		meshopt_optimizeVertexCache(&lod[0], &lod[0], lod.size(), mesh.vertices.size());
		meshopt_optimizeOverdraw(&lod[0], &lod[0], lod.size(), &mesh.vertices[0].px, mesh.vertices.size(), sizeof(Vertex), 1.0f);
	}

	// concatenate all LODs into one IB
	// note: the order of concatenation is important - since we optimize the entire IB for vertex fetch,
	// putting coarse LODs first makes sure that the vertex range referenced by them is as small as possible
	// some GPUs process the entire range referenced by the index buffer region so doing this optimizes the vertex transform
	// cost for coarse LODs
	size_t lod_index_offsets[lod_count] = {};
	size_t lod_index_counts[lod_count] = {};
	size_t total_index_count = 0;

	for (int i = lod_count - 1; i >= 0; --i)
	{
		lod_index_offsets[i] = total_index_count;
		lod_index_counts[i] = lods[i].size();

		total_index_count += lods[i].size();
	}

	mesh.indices.resize(total_index_count);

	for (size_t i = 0; i < lod_count; ++i)
	{
		memcpy(&mesh.indices[lod_index_offsets[i]], lods[i].data(), lods[i].size() * sizeof(lods[i][0]));
	}

	// vertex fetch optimization should go last as it depends on the final index order
	// note that the order of LODs above affects vertex fetch results
	meshopt_optimizeVertexFetch(&mesh.vertices[0], &mesh.indices[0], mesh.indices.size(), &mesh.vertices[0], mesh.vertices.size(), sizeof(Vertex));

	printf("%-9s:", "Simplify");

	for (size_t i = 0; i < sizeof(lods) / sizeof(lods[0]); ++i)
	{
		printf(" LOD%d %d", int(i), int(lods[i].size()) / 3);
	}

	printf("\n");

	// for using LOD data at runtime, in addition to VB and IB you have to save lod_index_offsets/lod_index_counts.
	(void)lod_index_offsets;
	(void)lod_index_counts;
}

void optimize(const Mesh& mesh, const char* name, void (*optf)(Mesh& mesh))
{
	Mesh copy = mesh;

	clock_t start = clock();
	optf(copy);
	clock_t end = clock();

	meshopt_VertexCacheStatistics vcs = meshopt_analyzeVertexCache(&copy.indices[0], copy.indices.size(), copy.vertices.size(), kCacheSize, 0, 0);
	meshopt_VertexFetchStatistics vfs = meshopt_analyzeVertexFetch(&copy.indices[0], copy.indices.size(), copy.vertices.size(), sizeof(Vertex));
	meshopt_OverdrawStatistics os = meshopt_analyzeOverdraw(&copy.indices[0], copy.indices.size(), &copy.vertices[0].px, copy.vertices.size(), sizeof(Vertex));

	meshopt_VertexCacheStatistics vcs_nv = meshopt_analyzeVertexCache(&copy.indices[0], copy.indices.size(), copy.vertices.size(), 32, 32, 32);
	meshopt_VertexCacheStatistics vcs_amd = meshopt_analyzeVertexCache(&copy.indices[0], copy.indices.size(), copy.vertices.size(), 14, 64, 128);
	meshopt_VertexCacheStatistics vcs_intel = meshopt_analyzeVertexCache(&copy.indices[0], copy.indices.size(), copy.vertices.size(), 128, 0, 0);

	printf("%-9s: ACMR %f ATVR %f (NV %f AMD %f Intel %f) Overfetch %f Overdraw %f in %.2f msec\n", name, vcs.acmr, vcs.atvr, vcs_nv.atvr, vcs_amd.atvr, vcs_intel.atvr, vfs.overfetch, os.overdraw, double(end - start) / CLOCKS_PER_SEC * 1000);
}

void encodeIndex(const Mesh& mesh)
{
	clock_t start = clock();

	std::vector<unsigned char> buffer(meshopt_encodeIndexBufferBound(mesh.indices.size(), mesh.vertices.size()));
	buffer.resize(meshopt_encodeIndexBuffer(&buffer[0], buffer.size(), &mesh.indices[0], mesh.indices.size()));

	clock_t middle = clock();

	std::vector<unsigned int> result(mesh.indices.size());
	int res = meshopt_decodeIndexBuffer(&result[0], mesh.indices.size(), &buffer[0], buffer.size());
	assert(res == 0);
	(void)res;

	clock_t end = clock();

	for (size_t i = 0; i < mesh.indices.size(); i += 3)
	{
		assert(
		    (result[i + 0] == mesh.indices[i + 0] && result[i + 1] == mesh.indices[i + 1] && result[i + 2] == mesh.indices[i + 2]) ||
		    (result[i + 1] == mesh.indices[i + 0] && result[i + 2] == mesh.indices[i + 1] && result[i + 0] == mesh.indices[i + 2]) ||
		    (result[i + 2] == mesh.indices[i + 0] && result[i + 0] == mesh.indices[i + 1] && result[i + 1] == mesh.indices[i + 2]));
	}

	printf("Index encode: %.1f bits/triangle; encode %.2f msec, decode %.2f msec (%.2f Mtri/s)\n",
	       double(buffer.size() * 8) / double(mesh.indices.size() / 3),
	       double(middle - start) / CLOCKS_PER_SEC * 1000,
	       double(end - middle) / CLOCKS_PER_SEC * 1000,
	       (double(result.size() / 3) / 1e6) / (double(end - middle) / CLOCKS_PER_SEC));
}

void stripify(const Mesh& mesh)
{
	clock_t start = clock();
	std::vector<unsigned int> strip(mesh.indices.size() / 3 * 4);
	strip.resize(meshopt_stripify(&strip[0], &mesh.indices[0], mesh.indices.size(), mesh.vertices.size()));
	clock_t end = clock();

	std::vector<unsigned int> list(mesh.indices.size());
	list.resize(meshopt_unstripify(&list[0], &strip[0], strip.size()));

	assert(list.size() == mesh.indices.size());

	meshopt_VertexCacheStatistics vcs = meshopt_analyzeVertexCache(&list[0], mesh.indices.size(), mesh.vertices.size(), kCacheSize, 0, 0);
	meshopt_VertexCacheStatistics vcs_nv = meshopt_analyzeVertexCache(&list[0], mesh.indices.size(), mesh.vertices.size(), 32, 32, 32);
	meshopt_VertexCacheStatistics vcs_amd = meshopt_analyzeVertexCache(&list[0], mesh.indices.size(), mesh.vertices.size(), 14, 64, 128);
	meshopt_VertexCacheStatistics vcs_intel = meshopt_analyzeVertexCache(&list[0], mesh.indices.size(), mesh.vertices.size(), 128, 0, 0);

	printf("Stripify : ACMR %f ATVR %f (NV %f AMD %f Intel %f); %d strip indices (%.1f%%) in %.2f msec\n",
	       vcs.acmr, vcs.atvr, vcs_nv.atvr, vcs_amd.atvr, vcs_intel.atvr,
	       int(strip.size()), double(strip.size()) / double(mesh.indices.size()) * 100,
	       double(end - start) / CLOCKS_PER_SEC * 1000);
}

void process(const char* path)
{
	Mesh mesh;

	if (path)
	{
		clock_t start = clock();
		clock_t middle;
		mesh = parseObj(path, middle);
		clock_t end = clock();

		if (mesh.vertices.empty())
		{
			printf("Mesh %s is empty, skipping\n", path);
			return;
		}

		printf("# %s: %d vertices, %d triangles; read in %.2f msec; indexed in %.2f msec\n", path, int(mesh.vertices.size()), int(mesh.indices.size() / 3), double(middle - start) / CLOCKS_PER_SEC * 1000, double(end - middle) / CLOCKS_PER_SEC * 1000);
	}
	else
	{
		mesh = generatePlane(1000);

		printf("# tessellated plane: %d vertices, %d triangles\n", int(mesh.vertices.size()), int(mesh.indices.size() / 3));
	}

	optimize(mesh, "Original", optNone);
	optimize(mesh, "Random", optRandomShuffle);
	optimize(mesh, "Cache", optCache);
	optimize(mesh, "CacheFifo", optCacheFifo);
	optimize(mesh, "Overdraw", optOverdraw);
	optimize(mesh, "Fetch", optFetch);
	optimize(mesh, "Complete", optComplete);

	// note: the ATVR/overdraw output from this pass is not necessarily correct since we analyze all LODs at once
	optimize(mesh, "Simplify", optCompleteSimplify);

	Mesh copy = mesh;
	meshopt_optimizeVertexCache(&copy.indices[0], &copy.indices[0], copy.indices.size(), copy.vertices.size());
	meshopt_optimizeVertexFetch(&copy.vertices[0], &copy.indices[0], copy.indices.size(), &copy.vertices[0], copy.vertices.size(), sizeof(Vertex));

	encodeIndex(copy);

	stripify(copy);
}

int main(int argc, char** argv)
{
	if (argc == 1)
	{
		printf("Usage: %s [.obj file]\n", argv[0]);
		process(0);
	}
	else
	{
		for (int i = 1; i < argc; ++i)
		{
			process(argv[i]);
		}
	}
}
