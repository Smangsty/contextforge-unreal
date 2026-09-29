import { build } from "esbuild";

await build({
  entryPoints: ["src/contextforge-unreal.mjs"],
  bundle: true,
  platform: "node",
  format: "esm",
  target: "node20",
  outfile: "bin/contextforge-unreal.mjs",
  legalComments: "none",
  minify: false,
  sourcemap: false,
  treeShaking: true
});
