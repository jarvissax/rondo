// Builds the update page into dist/: the bundled script, the page, its style and font.
//   node build.mjs            build
//   node build.mjs --serve    build, add the locally built bundle (tools/native/bundle.sh), and
//                             serve on http://localhost:8000 (WebUSB works on localhost)
import * as esbuild from "esbuild";
import { copyFileSync, existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from "node:fs";

const root = new URL("..", import.meta.url).pathname;
const out = new URL("dist/", import.meta.url).pathname;
mkdirSync(out + "fonts", { recursive: true });

const options = {
  entryPoints: ["src/main.js"],
  bundle: true,
  format: "esm",
  target: ["chrome100", "edge100"],
  minify: true,
  sourcemap: true,
  outfile: out + "app.js",
};
copyFileSync("index.html", out + "index.html");
copyFileSync("style.css", out + "style.css");
copyFileSync(root + "native/assets/fonts/Roboto-Regular.ttf", out + "fonts/Roboto-Regular.ttf");

if (process.argv.includes("--serve")) {
  const versionH = readFileSync(root + "native/core/version.h", "utf8");
  const version = versionH.match(/kRondoVersion = "([^"]+)"/)?.[1] ?? "dev";
  const firmware = Number(versionH.match(/kFirmwareNeeded = (\d+)/)?.[1] ?? 2);
  const bundle = root + `native/build/bundle/rondo-${version}.rondo`;
  if (existsSync(bundle)) {
    const size = statSync(bundle).size;
    copyFileSync(bundle, out + "rondo.rondo");
    // the release's latest.json (.github/workflows/release.yml)
    writeFileSync(out + "latest.json", JSON.stringify({ version, file: "rondo.rondo", size, bundle: "rondo.rondo", bundleSize: size, firmware }));
    console.log(`using the local bundle (Rondo ${version})`);
  }
  const ctx = await esbuild.context(options);
  await ctx.watch();
  const { port } = await ctx.serve({ servedir: out, port: 8000 });
  console.log(`http://localhost:${port}`);
} else {
  await esbuild.build(options);
  console.log("built dist/");
}
