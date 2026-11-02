// Builds the update page into dist/: the bundled script, the page, its style and font.
//   node build.mjs            build
//   node build.mjs --serve    build, add the locally built release APK, and serve on
//                             http://localhost:8000 (WebUSB works on localhost)
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
copyFileSync(root + "app/app/src/main/assets/fonts/Roboto-Regular.ttf", out + "fonts/Roboto-Regular.ttf");

if (process.argv.includes("--serve")) {
  const apk = root + "app/app/build/outputs/apk/release/app-release.apk";
  if (existsSync(apk)) {
    const version = readFileSync(root + "app/app/build.gradle.kts", "utf8").match(/versionName = "([^"]+)"/)?.[1] ?? "dev";
    copyFileSync(apk, out + "rondo.apk");
    writeFileSync(out + "latest.json", JSON.stringify({ version, file: "rondo.apk", size: statSync(apk).size }));
    console.log(`using the local release APK (Rondo ${version})`);
  }
  const ctx = await esbuild.context(options);
  await ctx.watch();
  const { port } = await ctx.serve({ servedir: out, port: 8000 });
  console.log(`http://localhost:${port}`);
} else {
  await esbuild.build(options);
  console.log("built dist/");
}
