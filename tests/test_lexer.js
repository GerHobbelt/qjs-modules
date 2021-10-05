import * as os from 'os';
import * as std from 'std';
import * as fs from 'fs';
import inspect from 'inspect';
import * as path from 'path';
import { Predicate } from 'predicate';
import { Location, Lexer, Token, SyntaxError } from 'lexer';
import { Console } from 'console';
import JSLexer from '../lib/jslexer.js';
import CLexer from '../lib/clexer.js';
import BNFLexer from '../lib/bnflexer.js';
import { escape, quote, toString } from 'misc';
import { define, curry, unique, split, extendArray } from 'util';

let buffers = {},
  modules = {};
let T;

('use strict');
('use math');

extendArray(Array.prototype);

const AddUnique = (arr, item) => (arr.indexOf(item) == -1 ? arr.push(item) : null);

const IntToDWord = ival => (isNaN(ival) === false && ival < 0 ? ival + 4294967296 : ival);
const IntToBinary = i => (i == -1 || typeof i != 'number' ? i : '0b' + IntToDWord(i).toString(2));

//const code = ["const str = stack.toString().replace(/\\n\\s*at /g, '\\n');", "/^(.*)\\s\\((.*):([0-9]*):([0-9]*)\\)$/.exec(line);" ];
const code = [
  "const str = stack.toString().replace(/\\n\\s*at /g, '\\n');",
  '/Reg.*Ex/i.test(n)',
  '/\\n/g',
  'const [match, pattern, flags] = /^\\/(.*)\\/([a-z]*)$/.exec(token.value);',
  '/^\\s\\((.*):([0-9]*):([0-9]*)\\)$/.exec(line);'
];

extendArray(Array.prototype);

const bufferRef = new WeakMap();

function BufferFile(file) {
  // console.log('BufferFile', file);
  if(buffers[file]) return buffers[file];
  let b = (buffers[file] = fs.readFileSync(file, { flag: 'r' }));
  bufferRef.set(b, file);
  return buffers[file];
}

function BufferLengths(file) {
  return buffers[file].map(b => b.byteLength);
}

function BufferOffsets(file) {
  return buffers[file].reduce(([pos, list], b) => [pos + b.byteLength, list.concat([pos])], [0, []])[1];
}

function BufferRanges(file) {
  return buffers[file].reduce(([pos, list], b) => [pos + b.byteLength, list.concat([[pos, b.byteLength]])], [0, []])[1];
}

function WriteFile(file, tok) {
  let f = std.open(file, 'w+');
  f.puts(tok);
  console.log('Wrote "' + file + '": ' + tok.length + ' bytes');
}

function DumpLexer(lex) {
  const { size, pos, start, line, column, lineStart, lineEnd, columnIndex } = lex;

  return 'Lexer ' + inspect({ start, pos, size });
}
function DumpToken(tok) {
  const { length, offset, chars, loc } = tok;

  return `★ Token ${inspect({ chars, offset, length, loc }, { depth: 1 })}`;
}
const What = {
  IMPORT: 0,
  EXPORT: 1
};

const ImportTypes = {
  IMPORT: 0,
  IMPORT_DEFAULT: 1,
  IMPORT_NAMESPACE: 2
};

const TokIs = curry((type, lexeme, tok) => {
  if(lexeme != undefined) {
    if(typeof lexeme == 'string' && tok.lexeme != lexeme) return false;
    else if(Array.isArray(lexeme) && lexeme.indexOf(tok.lexeme) == -1) return false;
  }
  if(type != undefined) {
    if(typeof type == 'string' && tok.type != type) return false;
    if(typeof type == 'number' && tok.id != type) return false;
  }
  return true;
});

const IsKeyword = TokIs('keyword');
const IsPunctuator = TokIs('punctuator');
const IsIdentifier = TokIs('identifier');
const IsStringLiteral = TokIs('stringLiteral');

function ImportType(seq) {
  if(IsKeyword('import', seq[0])) seq.shift();
  if(IsPunctuator('*', seq[0])) {
    if(IsKeyword('as', seq[1])) return ImportTypes.IMPORT_NAMESPACE;
  } else if(IsIdentifier(undefined, seq[0])) {
    if(IsKeyword('from', seq[1])) return ImportTypes.IMPORT_DEFAULT;
  }
  return ImportTypes.IMPORT;
}

function ImportFile(seq) {
  let idx = seq.findIndex(tok => IsKeyword('from', tok));
  while(seq[idx] && seq[idx].type != 'stringLiteral') ++idx;
  return seq[idx].lexeme.replace(/^['"`](.*)['"`]$/g, '$1');
}

function ExportName(seq) {
  let idx = seq.findIndex(tok => IsIdentifier(undefined, tok) || IsKeyword('default', tok));
  return seq[idx].lexeme;
}

function AddExport(tokens) {
  let code = tokens.map(tok => tok.lexeme).join('');
  tokens = tokens.filter(tok => tok.type != 'whitespace');
  let len = tokens.findIndex(tok => IsIdentifier(undefined, tok) || IsKeyword('default', tok)) + 1;
  tokens = tokens.slice(0, len);
  let exp = define(
    {
      type: tokens[1].lexeme,
      tokens,
      exported: ExportName(tokens),
      range: [+tokens[0].loc, +tokens.last.loc]
    },
    { code, loc: tokens[0].loc }
  );
  return exp;
}

function AddImport(tokens) {
  //console.log('tokens:', tokens);
  let range = [+tokens[0].loc, +tokens.last.loc];
  let code = tokens.map(tok => tok.lexeme).join('');
  tokens = tokens.filter(tok => tok.type != 'whitespace');
  let type = ImportType(tokens);
  let file = ImportFile(tokens);
  const { loc, seq } = tokens[0];
  let imp = define({ type, file, loc, seq, range }, { tokens, code });
  imp.local = {
    [ImportTypes.IMPORT_NAMESPACE]: () => {
      let idx = tokens.findIndex(tok => IsKeyword('as', tok));
      return tokens[idx + 1].lexeme;
    },
    [ImportTypes.IMPORT_DEFAULT]: () => {
      let idx = tokens.findIndex(tok => IsKeyword('import', tok));
      return tokens[idx + 1].lexeme;
    },
    [ImportTypes.IMPORT]: () => {
      let idx = 0;
      let specifier = [];
      let specifiers = [];
      if(IsKeyword('import', tokens[idx])) ++idx;
      if(IsPunctuator('{', tokens[idx])) ++idx;
      for(; !IsKeyword('from', tokens[idx]); ++idx) {
        if(IsPunctuator([',', '}'], tokens[idx])) {
          if(specifier.length) specifiers.push(specifier);
          specifier = [];
        } else {
          specifier.push(tokens[idx].lexeme);
        }
      }
      return specifiers;
    }
  }[type]();

  return imp;
}

function PrintES6Import(imp) {
  return {
    [ImportTypes.IMPORT_NAMESPACE]: ({ local, file }) => `import * as ${local} from '${file}';`,
    [ImportTypes.IMPORT_DEFAULT]: ({ local, file }) => `import ${local} from '${file}';`,
    [ImportTypes.IMPORT]: ({ local, file }) => `import { ${local.join(', ')} } from '${file}';`
  }[imp.type](imp);
}

function PrintCJSImport({ type, local, file }) {
  return {
    [ImportTypes.IMPORT_NAMESPACE]: () => `const ${local} = require('${file}');`,
    [ImportTypes.IMPORT_DEFAULT]: () => `const ${local} = require('${file}');`,
    [ImportTypes.IMPORT]: () => `const { ${local.join(', ')} } = require('${file}');`
  }[type]();
}

function main(...args) {
  globalThis.console = new Console({
    inspectOptions: {
      colors: true,
      depth: 8,
      breakLength: 160,
      maxStringLength: 100,
      maxArrayLength: 40,
      compact: 2,
      stringBreakNewline: false,
      hideKeys: [Symbol.toStringTag /*, 'code'*/]
    }
  });

  let optind = 0;
  let code = 'c';
  let debug;

  while(args[optind] && args[optind].startsWith('-')) {
    if(/code/.test(args[optind])) code = args[++optind];
    if(/(debug|^-x$)/.test(args[optind])) debug = true;

    optind++;
  }

  const RelativePath = file => path.join(path.dirname(process.argv[1]), '..', file);

  let files = args[optind] ? args.slice(optind) : [RelativePath('lib/util.js')];

  while(optind < files.length) {
    ProcessFile(files[optind++]);
  }

  function ProcessFile(file) {
    console.log(`Loading '${file}'...`);
    const log = (...args) => console.log(`${file}:`, ...args);

    let str = file ? BufferFile(file) : code[1];
    //str = '  return new Map(ret.map(([name, description]) => [name, { url: `https://github.com/${user}/${name}`, description }]));';
    let len = str.length;
    let type = path.extname(file).substring(1);
    log('data:', escape(str.slice(0, 100)));

    let lex = {
      js: new JSLexer(str, file),
      c: new CLexer(str, CLexer.LONGEST, file),
      bnf: new BNFLexer(str, file)
    };

    lex.g4 = lex.bnf;
    lex.ebnf = lex.bnf;
    lex.l = lex.bnf;
    lex.y = lex.bnf;

    const lexer = lex[type];

    T = lexer.tokens.reduce((acc, name, id) => ({ ...acc, [name]: id }), {});

    log('lexer.tokens:', lexer.tokens);
    log('lexer:', lexer[Symbol.toStringTag]);
    log('code:', code);

    let e = new SyntaxError();
    log('new SyntaxError()', e);

    lexer.handler = lex => {
      const { loc, mode, pos, start, byteLength, state } = lex;
      //log(`${this.currentLine()}`);
      //log(`handler loc=${loc} mode=${IntToBinary(mode)} state=${lex.topState()}`, { pos, start, byteLength }, `\n${lex.currentLine()}` );
      log(' '.repeat(loc.column - 1) + '^');
    };
    let tokenList = [],
      declarations = [];
    const colSizes = [12, 8, 4, 16, 32, 10, 0];

    const printTok = debug
      ? (tok, prefix) => {
          const range = tok.charRange;
          const cols = [prefix, `tok[${tok.byteLength}]`, tok.id, tok.type, tok.lexeme, tok.lexeme.length, tok.loc];
          log(...cols.map((col, i) => (col + '').replaceAll('\n', '\\n').padEnd(colSizes[i])));
        }
      : () => {};

    let tok,
      i = 0;

    log('now', Date.now());

    log(lexer.ruleNames.length, 'rules', lexer.ruleNames.unique().length, 'unique rules');

    log('lexer.mask', IntToBinary(lexer.mask));
    log('lexer.skip', lexer.skip);
    log('lexer.skip', IntToBinary(lexer.skip));
    log('lexer.states', lexer.states);

    log('new SyntaxError("test")', new SyntaxError('test', new Location(10, 3, 28, 'file.txt')));
    let mask = IntToBinary(lexer.mask);
    let state = lexer.topState();
    lexer.beginCode = () => (code == 'js' ? 0b1000 : 0b0100);
    let tokens = [];
    let start = Date.now();
    const balancer = () => {
      let self;
      let stack = [];
      const table = { '}': '{', ']': '[', ')': '(' };
      self = function ParentheseBalancer(tok) {
        switch (tok?.lexeme) {
          case '{':
          case '[':
          case '(': {
            stack.push(tok.lexeme);
            break;
          }
          case '}':
          case ']':
          case ')': {
            if(stack.last != table[tok.lexeme]) throw new Error(`top '${stack.last}' != '${tok.lexeme}' [ ${stack.map(s => `'${s}'`).join(', ')} ]`);

            stack.pop();
            break;
          }
        }
      };
      Object.assign(self, {
        stack,
        reset() {
          stack.clear();
        },
        /* prettier-ignore */ get depth() {
        return stack.length;
      }
      });

      return self;
    };
    let balancers = [balancer()];

    let imports = [],
      exports = [];
    let impexp,
      cond,
      imp = [];

    for(;;) {
      let newState, state;
      let { stateDepth } = lexer;
      state = lexer.topState();
      let { done, value } = lexer.next();
      if(done) break;
      newState = lexer.topState();
      tok = value;
      console.log('token', { lexeme: tok.lexeme, id: tok.id, loc: tok.loc + '' });
      if(newState != state) {
        if(state == 'TEMPLATE' && lexer.stateDepth > stateDepth) balancers.push(balancer());
        if(newState == 'TEMPLATE' && lexer.stateDepth < stateDepth) balancers.pop();
      }
      let n = balancers.last.depth;
      if(n == 0 && tok.lexeme == '}' && lexer.stateDepth > 0) {
        lexer.popState();
        continue;
      } else {
        balancer(tok);
        if(n > 0 && balancers.last.depth == 0) log('balancer');
      }
      if(['import', 'export'].indexOf(tok.lexeme) >= 0) {
        impexp = What[tok.lexeme.toUpperCase()];
        let prev = tokens[tokens.length - 1];
        /* if(tokens.length == 0 || prev.lexeme.endsWith('\n')) */ {
          cond = true;
          imp = [];
        }
      }
      if(cond == true) {
        imp.push(tok);
        if([';', '\n'].indexOf(tok.lexeme) != -1) {
          cond = false;
          if(imp.some(i => i.lexeme == 'from')) {
            if(impexp == What.IMPORT) imports.push(AddImport(imp));
          }

          if(impexp == What.EXPORT) exports.push(AddExport(imp));
        }
      }
      printTok(tok, lexer.topState());
      tokens.push(tok);
    }

    log('ES6 imports', imports.map(PrintES6Import));
    log('CJS imports', imports.map(PrintCJSImport));

    modules[file] = { imports, exports };

    //  log('exports', exports);
    let fileImports = imports.filter(imp => /\.js$/i.test(imp.file));
    let splitPoints = unique(fileImports.reduce((acc, imp) => [...acc, ...imp.range], []));
    buffers[file] = [...split(BufferFile(file), ...splitPoints)].map(b => b ?? toString(b, 0, b.byteLength));
    log(`splitPoints`, splitPoints);
    log(`buffers[${file}]`, buffers[file]);
    log(`buffers[${file}] len`, BufferLengths(file));
    log(`buffers[${file}] ofs`, BufferOffsets(file));
    log(`buffers[${file}] rng`, BufferRanges(file));
    log(
      'fileImports',
      fileImports.map(imp => imp.file)
    );

    let dir = path.dirname(file);

    fileImports.forEach(imp => {
      let p = path.collapse(path.join(dir, imp.file));
      log('p', p);

      AddUnique(files, p);
      //    let f = BufferFile(p);
    });

    let end = Date.now();

    log(`took ${end - start}ms`);
    log('lexer', lexer);
    //log('tokens', tokens);

    std.gc();
  }
  console.log('buffers', buffers);
  console.log('files', files);
}

try {
  main(...scriptArgs.slice(1));
} catch(error) {
  console.log(`FAIL: ${error.message}\n${error.stack}`);
  std.exit(1);
} finally {
  console.log('SUCCESS');
}
