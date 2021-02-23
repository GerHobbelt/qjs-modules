import * as os from 'os';
import * as std from 'std';
import inspect from 'inspect.so';
import * as xml from 'xml.so';
import { Pointer } from 'pointer.so';
import { TreeWalker } from 'tree-walker.so';
import Console from './console.js';

function WriteFile(file, data) {
  let f = std.open(file, 'w+');
  f.puts(data);
  console.log(`Wrote '${file}': ${data.length} bytes`);
}

function main(...args) {
  new Console({});
  
  let data = std.loadFile(args[0] ?? std.getenv('HOME') + '/Sources/an-tronics/eagle/FM-Radio-Simple-Receiver-Dip1.sch',
    'utf-8'
  );

  console.log('data:', data.length);

  let result = xml.read(data);
  console.log('result:', result);

  console.log('xml:',
    inspect(result.slice(0, 2), { depth: Infinity, compact: Infinity, colors: true })
  );

  let walker = new TreeWalker(result);
  let node;
  while((node = walker.nextNode())) {
    console.log('node:', node.toString());
    console.log('path:', node.currentPath);
  }

  WriteFile('output.json', JSON.stringify(result, null, 2));

  std.gc();
}

main(scriptArgs.slice(1));
