#!/usr/bin/env python3
"""
side_by_side_app.py - Advanced Interactive Web Interface for SQL Query Engine
Provides real-time side-by-side execution, correctness verification against SQLite oracle,
code editor with SQL syntax highlighting and autocomplete, execution time comparison,
and speedup analytics.
"""

import csv
import http.server
import io
import json
import math
import os
import re
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.parse

# Ensure local tools directory is importable
script_dir = os.path.dirname(os.path.abspath(__file__))
if script_dir not in sys.path:
    sys.path.insert(0, script_dir)

from compare import load_csv_into_sqlite, cells_match, rows_match, make_row_sort_key

HTML_TEMPLATE = r"""<!DOCTYPE html>
<html lang="en" class="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>SQL Engine vs SQLite - High Performance Side-by-Side Runner</title>
  <!-- Tailwind CSS CDN -->
  <script src="https://www.gstatic.com/antigravity/web/dev/tailwindcss.min.js"></script>
  <!-- CodeMirror CSS & Themes -->
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.css">
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/theme/dracula.min.css">
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/hint/show-hint.min.css">
  <!-- CodeMirror JS & SQL Mode & Addons -->
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/codemirror.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/mode/sql/sql.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/hint/show-hint.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/hint/sql-hint.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/edit/matchbrackets.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.65.16/addon/edit/closebrackets.min.js"></script>

  <style>
    :root {
      --bg-page: #090d16;
      --card-bg: #111827;
      --border: #1f2937;
    }
    body {
      background-color: var(--bg-page);
      color: #f1f5f9;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
    }
    .code-font {
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
    }
    /* CodeMirror dark theme customization */
    .CodeMirror {
      height: 180px;
      font-family: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, monospace;
      font-size: 13.5px;
      border-radius: 0.5rem;
      padding: 6px 0;
      background: #0b1120 !important;
      border: 1px solid #1e293b;
      transition: border-color 0.15s ease-in-out;
    }
    .CodeMirror-focused {
      border-color: #38bdf8 !important;
      box-shadow: 0 0 0 1px #38bdf8;
    }
    .CodeMirror-gutters {
      background: #0b1120 !important;
      border-right: 1px solid #1e293b;
    }
    .CodeMirror-linenumber {
      color: #475569 !important;
    }
    /* Custom Scrollbars */
    ::-webkit-scrollbar {
      width: 7px;
      height: 7px;
    }
    ::-webkit-scrollbar-track {
      background: #0f172a;
    }
    ::-webkit-scrollbar-thumb {
      background: #334155;
      border-radius: 4px;
    }
    ::-webkit-scrollbar-thumb:hover {
      background: #475569;
    }
    .badge-null {
      font-size: 10px;
      font-weight: 600;
      padding: 1px 5px;
      border-radius: 4px;
      background: rgba(148, 163, 184, 0.15);
      color: #94a3b8;
      border: 1px solid rgba(148, 163, 184, 0.25);
      font-style: italic;
    }
    .animate-pulse-slow {
      animation: pulse 2.5s cubic-bezier(0.4, 0, 0.6, 1) infinite;
    }
  </style>
</head>
<body class="p-4 md:p-6 max-w-7xl mx-auto min-h-screen flex flex-col">

  <!-- Header -->
  <header class="mb-5 border-b border-slate-800 pb-4 flex flex-col md:flex-row justify-between items-start md:items-center gap-3">
    <div>
      <div class="flex items-center gap-2.5">
        <div class="w-9 h-9 rounded-xl bg-gradient-to-tr from-sky-600 to-indigo-500 flex items-center justify-center shadow-lg shadow-sky-500/20">
          <span class="text-lg">⚡</span>
        </div>
        <div>
          <h1 class="text-xl font-bold tracking-tight text-white flex items-center gap-2">
            SQL Query Engine <span class="text-xs px-2 py-0.5 rounded-full bg-sky-500/20 text-sky-400 border border-sky-500/30 font-mono">v1.0 (Phase 8)</span>
          </h1>
          <p class="text-xs text-slate-400">High-performance C++17 Engine vs. SQLite 3 Oracle with Latency & Speedup Analytics</p>
        </div>
      </div>
    </div>
    <div class="flex flex-wrap items-center gap-2 text-xs">
      <div class="flex items-center gap-1.5 bg-slate-900 border border-slate-700/70 px-2.5 py-1 rounded-lg">
        <span class="w-2 h-2 rounded-full bg-sky-400 animate-pulse"></span>
        <span class="text-slate-300 font-medium">Custom C++17 Engine</span>
      </div>
      <div class="flex items-center gap-1.5 bg-slate-900 border border-slate-700/70 px-2.5 py-1 rounded-lg">
        <span class="w-2 h-2 rounded-full bg-blue-400"></span>
        <span class="text-slate-300 font-medium">SQLite Oracle</span>
      </div>
      <div class="text-slate-500 hidden sm:inline">|</div>
      <kbd class="hidden sm:inline-block bg-slate-800 text-slate-400 px-2 py-0.5 rounded border border-slate-700 text-[11px] code-font">Ctrl + Enter to run</kbd>
    </div>
  </header>

  <!-- Query Editor & Controls Card -->
  <div class="bg-slate-900/90 border border-slate-800 rounded-xl p-4 md:p-5 mb-5 shadow-2xl backdrop-blur">
    <!-- Editor Header Toolbar -->
    <div class="flex flex-col sm:flex-row justify-between items-start sm:items-center gap-2.5 mb-2.5">
      <div class="flex items-center gap-2">
        <span class="text-xs font-semibold text-sky-400 uppercase tracking-wider flex items-center gap-1.5">
          <span>📝</span> SQL Editor
        </span>
        <button onclick="toggleSchemaDrawer()" id="schemaToggleBtn" class="text-xs bg-slate-800 hover:bg-slate-700 text-slate-300 px-2.5 py-1 rounded border border-slate-700 transition flex items-center gap-1">
          <span>🗄️</span> Schema Explorer
        </button>
      </div>

      <div class="flex flex-wrap items-center gap-2 w-full sm:w-auto">
        <!-- Preset Queries Dropdown -->
        <select id="presetSelect" onchange="loadPreset()" class="text-xs bg-slate-950 border border-slate-700 text-slate-200 rounded-lg px-2.5 py-1.5 focus:outline-none focus:border-sky-500 transition max-w-xs">
          <option value="">-- Load Preset Query --</option>
          <optgroup label="Single Table Scans & Filters (t)">
            <option value="S1">S1: SELECT COUNT(*) FROM t (Scan floor)</option>
            <option value="S2">S2: SELECT COUNT(*) WHERE qty > 5 AND amount < 100</option>
            <option value="S3">S3: SELECT SUM(amount), AVG(qty), MIN(id), MAX(id)</option>
            <option value="S9">S9: SELECT COUNT(*) WHERE note LIKE '%urgent%'</option>
          </optgroup>
          <optgroup label="Group By & Aggregations (t)">
            <option value="S4" selected>S4: String Group By & Agg (city, COUNT, SUM)</option>
            <option value="S5">S5: Multi-key Group By (region, qty, COUNT)</option>
            <option value="S8">S8: SELECT DISTINCT city FROM t</option>
          </optgroup>
          <optgroup label="Sorting & Top-N (t)">
            <option value="S6">S6: Top-10 ORDER BY amount DESC, id</option>
            <option value="S7">S7: Full Sort ORDER BY amount DESC, id LIMIT 25</option>
          </optgroup>
          <optgroup label="Multi-Table Joins (orders, customers, products)">
            <option value="J1">J1: 2-Table INNER JOIN (orders + customers)</option>
            <option value="J2">J2: JOIN + GROUP BY (orders + customers by region)</option>
            <option value="J3">J3: 3-Table JOIN + Top-N (orders + customers + products)</option>
            <option value="J4">J4: LEFT ANTI-JOIN (orders without customer)</option>
            <option value="J6">J6: FULL OUTER JOIN (Phase 5B)</option>
          </optgroup>
        </select>

        <!-- Dataset Dropdown -->
        <select id="datasetSelect" onchange="onDatasetChange()" class="text-xs bg-slate-950 border border-slate-700 text-slate-200 rounded-lg px-2.5 py-1.5 focus:outline-none focus:border-sky-500 transition">
          <option value="100k">Dataset: 100k rows (data_100k.csv)</option>
          <option value="small">Dataset: 10 rows (data_small.csv)</option>
          <option value="5m">Dataset: 5M rows (data_5m.csv)</option>
        </select>
      </div>
    </div>

    <!-- Code Editor Container -->
    <div class="relative">
      <textarea id="sqlInput" class="w-full bg-slate-950 border border-slate-700 rounded-lg p-3 text-xs md:text-sm code-font text-slate-100 focus:outline-none focus:border-sky-500 transition" placeholder="SELECT ...">SELECT city, COUNT(*), SUM(amount) FROM t GROUP BY city ORDER BY city LIMIT 10</textarea>
    </div>

    <!-- Action Toolbar Below Editor -->
    <div class="mt-3 flex flex-col sm:flex-row justify-between items-start sm:items-center gap-3">
      <!-- Quick Editor Buttons -->
      <div class="flex flex-wrap items-center gap-2">
        <button onclick="formatSQL()" title="Format and capitalize SQL" class="text-xs bg-slate-800 hover:bg-slate-700 text-slate-300 px-2.5 py-1 rounded border border-slate-700 transition flex items-center gap-1">
          <span>✨</span> Format SQL
        </button>
        <button onclick="explainPlan()" title="Show Physical Operator Tree" class="text-xs bg-slate-800 hover:bg-slate-700 text-slate-300 px-2.5 py-1 rounded border border-slate-700 transition flex items-center gap-1">
          <span>🔍</span> Explain Plan
        </button>
        <button onclick="clearEditor()" title="Clear editor" class="text-xs bg-slate-800 hover:bg-slate-700 text-slate-400 hover:text-slate-200 px-2 py-1 rounded border border-slate-700 transition">
          Clear
        </button>
        <button onclick="copySQL()" title="Copy SQL to clipboard" class="text-xs bg-slate-800 hover:bg-slate-700 text-slate-400 hover:text-slate-200 px-2 py-1 rounded border border-slate-700 transition">
          Copy
        </button>
      </div>

      <!-- Execution Button -->
      <div class="flex items-center gap-2 w-full sm:w-auto justify-end">
        <span id="runStatusText" class="text-xs text-slate-400 hidden sm:inline"></span>
        <button id="runBtn" onclick="runQuery()" class="w-full sm:w-auto bg-gradient-to-r from-sky-500 to-indigo-600 hover:from-sky-400 hover:to-indigo-500 text-white font-semibold text-sm px-6 py-2.5 rounded-lg transition-all shadow-lg shadow-sky-500/25 flex items-center justify-center gap-2">
          <span id="runBtnIcon">▶</span> <span id="runBtnText">Run & Compare</span>
        </button>
      </div>
    </div>

    <!-- Schema Explorer Drawer (Collapsible) -->
    <div id="schemaDrawer" class="hidden mt-4 pt-4 border-t border-slate-800">
      <div class="flex justify-between items-center mb-2">
        <span class="text-xs font-semibold text-slate-300 uppercase tracking-wider">Available Tables & Schema (Click column to insert)</span>
        <span class="text-[11px] text-slate-500">Auto-mapped by query engine</span>
      </div>
      <div class="grid grid-cols-1 md:grid-cols-4 gap-3 text-xs">
        <!-- Table t -->
        <div class="bg-slate-950/80 border border-slate-800 rounded-lg p-2.5">
          <div class="font-bold text-sky-400 mb-1 flex justify-between">
            <span class="cursor-pointer hover:underline" onclick="insertSnippet('t')">Table: t</span>
            <span class="text-[10px] text-slate-500 font-normal">Active Dataset</span>
          </div>
          <div class="space-y-0.5 text-slate-300">
            <div class="cursor-pointer hover:text-sky-300" onclick="insertSnippet('id')"><span class="text-slate-500">INT</span> id</div>
            <div class="cursor-pointer hover:text-sky-300" onclick="insertSnippet('city')"><span class="text-slate-500">STR</span> city</div>
            <div class="cursor-pointer hover:text-sky-300" onclick="insertSnippet('region')"><span class="text-slate-500">STR</span> region</div>
            <div class="cursor-pointer hover:text-sky-300" onclick="insertSnippet('qty')"><span class="text-slate-500">INT</span> qty</div>
            <div class="cursor-pointer hover:text-sky-300" onclick="insertSnippet('amount')"><span class="text-slate-500">FLT</span> amount</div>
            <div class="cursor-pointer hover:text-sky-300" onclick="insertSnippet('note')"><span class="text-slate-500">STR</span> note</div>
          </div>
        </div>
        <!-- Table orders -->
        <div class="bg-slate-950/80 border border-slate-800 rounded-lg p-2.5">
          <div class="font-bold text-indigo-400 mb-1 flex justify-between">
            <span class="cursor-pointer hover:underline" onclick="insertSnippet('orders')">Table: orders (o)</span>
            <span class="text-[10px] text-slate-500 font-normal">1,000 rows</span>
          </div>
          <div class="space-y-0.5 text-slate-300">
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('id')"><span class="text-slate-500">INT</span> id</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('cust_id')"><span class="text-slate-500">INT</span> cust_id</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('product_id')"><span class="text-slate-500">INT</span> product_id</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('qty')"><span class="text-slate-500">INT</span> qty</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('amount')"><span class="text-slate-500">FLT</span> amount</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('status')"><span class="text-slate-500">STR</span> status</div>
          </div>
        </div>
        <!-- Table customers -->
        <div class="bg-slate-950/80 border border-slate-800 rounded-lg p-2.5">
          <div class="font-bold text-indigo-400 mb-1 flex justify-between">
            <span class="cursor-pointer hover:underline" onclick="insertSnippet('customers')">Table: customers (c)</span>
            <span class="text-[10px] text-slate-500 font-normal">250 rows</span>
          </div>
          <div class="space-y-0.5 text-slate-300">
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('id')"><span class="text-slate-500">INT</span> id</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('name')"><span class="text-slate-500">STR</span> name</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('region')"><span class="text-slate-500">STR</span> region</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('code')"><span class="text-slate-500">STR</span> code</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('note')"><span class="text-slate-500">STR</span> note</div>
          </div>
        </div>
        <!-- Table products -->
        <div class="bg-slate-950/80 border border-slate-800 rounded-lg p-2.5">
          <div class="font-bold text-indigo-400 mb-1 flex justify-between">
            <span class="cursor-pointer hover:underline" onclick="insertSnippet('products')">Table: products (p)</span>
            <span class="text-[10px] text-slate-500 font-normal">50 rows</span>
          </div>
          <div class="space-y-0.5 text-slate-300">
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('id')"><span class="text-slate-500">INT</span> id</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('name')"><span class="text-slate-500">STR</span> name</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('price')"><span class="text-slate-500">FLT</span> price</div>
            <div class="cursor-pointer hover:text-indigo-300" onclick="insertSnippet('category')"><span class="text-slate-500">STR</span> category</div>
          </div>
        </div>
      </div>
    </div>
  </div>

  <!-- Error Alert Banner -->
  <div id="errorAlert" class="hidden mb-6 bg-rose-950/70 border border-rose-700/80 rounded-xl p-4 text-rose-200 shadow-xl">
    <div class="flex items-start gap-3">
      <span class="text-rose-400 text-lg">⚠️</span>
      <div class="flex-1">
        <h3 class="font-bold text-sm text-rose-300" id="errorTitle">Execution Error</h3>
        <pre id="errorMsg" class="mt-1 text-xs code-font text-rose-200/90 whitespace-pre-wrap overflow-x-auto"></pre>
      </div>
      <button onclick="document.getElementById('errorAlert').classList.add('hidden')" class="text-rose-400 hover:text-rose-200 text-xs">✕</button>
    </div>
  </div>

  <!-- Performance & Speedup Analytics Dashboard -->
  <div id="statsBar" class="hidden mb-6">
    <div class="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-4 mb-4">
      <!-- Engine Latency -->
      <div class="bg-slate-900 border border-slate-800 rounded-xl p-4 shadow-lg relative overflow-hidden">
        <div class="flex justify-between items-center text-xs text-slate-400 uppercase tracking-wider">
          <span>Engine Latency</span>
          <span class="text-sky-400 font-bold">C++17</span>
        </div>
        <div id="engTime" class="text-3xl font-extrabold text-sky-400 mt-1.5 code-font">-</div>
        <div class="flex justify-between items-center mt-1 text-xs text-slate-400">
          <span id="engRows">- rows</span>
          <span id="engRate" class="text-slate-500"></span>
        </div>
        <div class="absolute bottom-0 left-0 right-0 h-1 bg-sky-500"></div>
      </div>

      <!-- SQLite Latency -->
      <div class="bg-slate-900 border border-slate-800 rounded-xl p-4 shadow-lg relative overflow-hidden">
        <div class="flex justify-between items-center text-xs text-slate-400 uppercase tracking-wider">
          <span>SQLite Latency</span>
          <span class="text-indigo-400 font-bold">Oracle</span>
        </div>
        <div id="sqTime" class="text-3xl font-extrabold text-indigo-400 mt-1.5 code-font">-</div>
        <div class="flex justify-between items-center mt-1 text-xs text-slate-400">
          <span id="sqRows">- rows</span>
          <span class="text-slate-500">In-memory</span>
        </div>
        <div class="absolute bottom-0 left-0 right-0 h-1 bg-indigo-500"></div>
      </div>

      <!-- Speedup Comparison Card -->
      <div class="bg-slate-900 border border-slate-800 rounded-xl p-4 shadow-lg relative overflow-hidden">
        <div class="flex justify-between items-center text-xs text-slate-400 uppercase tracking-wider">
          <span>Speedup Factor</span>
          <span id="speedupIcon" class="text-emerald-400">🚀</span>
        </div>
        <div id="speedup" class="text-3xl font-extrabold text-emerald-400 mt-1.5 code-font">-</div>
        <div class="flex justify-between items-center mt-1 text-xs">
          <span id="speedupLabel" class="text-slate-400 font-medium">vs SQLite</span>
          <span id="speedupPct" class="text-emerald-400 font-semibold"></span>
        </div>
        <div id="speedupBarBottom" class="absolute bottom-0 left-0 right-0 h-1 bg-emerald-500"></div>
      </div>

      <!-- Correctness Verification -->
      <div class="bg-slate-900 border border-slate-800 rounded-xl p-4 shadow-lg relative overflow-hidden">
        <div class="flex justify-between items-center text-xs text-slate-400 uppercase tracking-wider">
          <span>Verification</span>
          <span class="text-xs text-slate-500 font-mono">tol: 1e-6</span>
        </div>
        <div id="matchBadge" class="text-xl font-extrabold text-emerald-400 mt-2 flex items-center gap-1.5">
          <span>✓</span> EXACT MATCH
        </div>
        <div id="matchDesc" class="text-xs text-slate-400 mt-1 truncate" title="">Outputs match SQLite oracle</div>
        <div id="matchBarBottom" class="absolute bottom-0 left-0 right-0 h-1 bg-emerald-500"></div>
      </div>
    </div>

    <!-- Relative Latency Progress Bar -->
    <div class="bg-slate-900/80 border border-slate-800 rounded-xl p-3.5 shadow-md">
      <div class="flex justify-between text-xs text-slate-400 mb-1.5">
        <span class="flex items-center gap-1.5"><span class="w-2.5 h-2.5 rounded bg-sky-400 inline-block"></span> Query Engine Latency: <strong id="barEngVal" class="text-sky-300"></strong></span>
        <span class="flex items-center gap-1.5"><strong id="barSqVal" class="text-indigo-300"></strong> :SQLite Oracle Latency <span class="w-2.5 h-2.5 rounded bg-indigo-500 inline-block"></span></span>
      </div>
      <div class="w-full bg-slate-950 rounded-full h-3 flex overflow-hidden p-0.5 border border-slate-800">
        <div id="barEng" class="bg-sky-400 h-full rounded-l transition-all duration-500" style="width: 50%"></div>
        <div id="barSq" class="bg-indigo-500 h-full rounded-r transition-all duration-500" style="width: 50%"></div>
      </div>
    </div>
  </div>

  <!-- Results Section -->
  <div id="resultsContainer" class="hidden flex-1 flex flex-col">
    <!-- View Switcher Tabs -->
    <div class="flex justify-between items-center border-b border-slate-800 mb-4 pb-2">
      <div class="flex gap-2">
        <button id="tabSideBySide" onclick="switchView('sideBySide')" class="text-xs font-semibold px-3 py-1.5 rounded-lg bg-sky-500/20 text-sky-400 border border-sky-500/30 transition flex items-center gap-1.5">
          <span>◫</span> Side-by-Side View
        </button>
        <button id="tabDiff" onclick="switchView('diff')" class="text-xs font-semibold px-3 py-1.5 rounded-lg bg-slate-800 text-slate-400 hover:text-slate-200 border border-slate-700 transition flex items-center gap-1.5">
          <span>⚖️</span> Unified Diff View
        </button>
        <button id="tabExplain" onclick="switchView('explain')" class="text-xs font-semibold px-3 py-1.5 rounded-lg bg-slate-800 text-slate-400 hover:text-slate-200 border border-slate-700 transition flex items-center gap-1.5">
          <span>🌳</span> Physical Operator Plan
        </button>
      </div>

      <div class="text-xs text-slate-400 flex items-center gap-3">
        <span id="totalRowsBadge" class="bg-slate-800 px-2.5 py-1 rounded border border-slate-700"></span>
      </div>
    </div>

    <!-- 1. Side-by-Side Split View Grid -->
    <div id="viewSideBySide" class="grid grid-cols-1 lg:grid-cols-2 gap-5 flex-1">
      <!-- Engine Output Box -->
      <div class="bg-slate-900 border border-slate-800 rounded-xl overflow-hidden flex flex-col shadow-xl">
        <div class="bg-slate-950/80 px-4 py-2.5 border-b border-slate-800 flex justify-between items-center">
          <div class="flex items-center gap-2">
            <span class="w-2.5 h-2.5 rounded-full bg-sky-400"></span>
            <span class="text-xs font-bold text-sky-400 uppercase tracking-wider">Custom Engine Solution</span>
            <span id="engHeaderTime" class="text-[11px] text-slate-500 code-font"></span>
          </div>
          <div class="flex items-center gap-2">
            <input type="text" id="engSearch" oninput="filterTable('engTable', this.value)" placeholder="Filter..." class="text-[11px] bg-slate-900 border border-slate-700 rounded px-2 py-0.5 text-slate-300 w-24 focus:w-36 transition-all focus:outline-none">
            <button onclick="copyTableCSV('eng')" title="Copy as CSV" class="text-slate-400 hover:text-slate-200 text-xs px-1.5 py-0.5 bg-slate-800 hover:bg-slate-700 rounded border border-slate-700">CSV</button>
          </div>
        </div>
        <div class="p-3 overflow-x-auto max-h-[520px] overflow-y-auto code-font text-xs flex-1">
          <table id="engTable" class="w-full text-left border-collapse"></table>
        </div>
      </div>

      <!-- SQLite Output Box -->
      <div class="bg-slate-900 border border-slate-800 rounded-xl overflow-hidden flex flex-col shadow-xl">
        <div class="bg-slate-950/80 px-4 py-2.5 border-b border-slate-800 flex justify-between items-center">
          <div class="flex items-center gap-2">
            <span class="w-2.5 h-2.5 rounded-full bg-indigo-400"></span>
            <span class="text-xs font-bold text-indigo-400 uppercase tracking-wider">SQLite Oracle Solution</span>
            <span id="sqHeaderTime" class="text-[11px] text-slate-500 code-font"></span>
          </div>
          <div class="flex items-center gap-2">
            <input type="text" id="sqSearch" oninput="filterTable('sqTable', this.value)" placeholder="Filter..." class="text-[11px] bg-slate-900 border border-slate-700 rounded px-2 py-0.5 text-slate-300 w-24 focus:w-36 transition-all focus:outline-none">
            <button onclick="copyTableCSV('sq')" title="Copy as CSV" class="text-slate-400 hover:text-slate-200 text-xs px-1.5 py-0.5 bg-slate-800 hover:bg-slate-700 rounded border border-slate-700">CSV</button>
          </div>
        </div>
        <div class="p-3 overflow-x-auto max-h-[520px] overflow-y-auto code-font text-xs flex-1">
          <table id="sqTable" class="w-full text-left border-collapse"></table>
        </div>
      </div>
    </div>

    <!-- 2. Unified Diff View -->
    <div id="viewDiff" class="hidden bg-slate-900 border border-slate-800 rounded-xl overflow-hidden flex flex-col shadow-xl">
      <div class="bg-slate-950/80 px-4 py-2.5 border-b border-slate-800 flex justify-between items-center">
        <span class="text-xs font-bold text-slate-300 uppercase tracking-wider">Row-by-Row Comparison (Engine vs SQLite)</span>
        <span id="diffSummaryBadge" class="text-xs font-mono text-emerald-400"></span>
      </div>
      <div class="p-3 overflow-x-auto max-h-[550px] overflow-y-auto code-font text-xs">
        <table id="diffTable" class="w-full text-left border-collapse"></table>
      </div>
    </div>

    <!-- 3. Physical Operator Plan View -->
    <div id="viewExplain" class="hidden bg-slate-900 border border-slate-800 rounded-xl overflow-hidden flex flex-col shadow-xl p-5">
      <div class="flex justify-between items-center mb-3">
        <span class="text-xs font-bold text-sky-400 uppercase tracking-wider">Physical Query Execution Plan</span>
        <span class="text-[11px] text-slate-500 font-mono">Engine Operator Tree (--explain)</span>
      </div>
      <pre id="explainOutput" class="code-font text-xs bg-slate-950 p-4 rounded-lg border border-slate-800 text-sky-300 overflow-x-auto leading-relaxed">Loading plan...</pre>
    </div>
  </div>

  <!-- Toast Notification -->
  <div id="toast" class="fixed bottom-6 right-6 bg-slate-800 text-slate-100 text-xs px-4 py-2.5 rounded-lg border border-slate-700 shadow-2xl transition-all duration-300 transform translate-y-12 opacity-0 pointer-events-none z-50 flex items-center gap-2">
    <span id="toastIcon">ℹ️</span> <span id="toastMsg">Message</span>
  </div>

  <script>
    // Presets library
    const PRESETS = {
      "S1": "SELECT COUNT(*) FROM t",
      "S2": "SELECT COUNT(*) FROM t WHERE qty > 5 AND amount < 100",
      "S3": "SELECT SUM(amount), AVG(qty), MIN(id), MAX(id) FROM t",
      "S4": "SELECT city, COUNT(*), SUM(amount) FROM t GROUP BY city ORDER BY city LIMIT 10",
      "S5": "SELECT region, qty, COUNT(*) FROM t GROUP BY region, qty ORDER BY region, qty LIMIT 20",
      "S6": "SELECT id, amount FROM t ORDER BY amount DESC, id LIMIT 10",
      "S7": "SELECT id, city, amount FROM t ORDER BY amount DESC, id LIMIT 25",
      "S8": "SELECT DISTINCT city FROM t ORDER BY city LIMIT 20",
      "S9": "SELECT COUNT(*) FROM t WHERE note LIKE '%urgent%'",
      "J1": "SELECT COUNT(*) FROM orders o JOIN customers c ON o.cust_id = c.id",
      "J2": "SELECT c.region, COUNT(*), SUM(o.amount) FROM orders o JOIN customers c ON o.cust_id = c.id GROUP BY c.region ORDER BY c.region",
      "J3": "SELECT c.name, COUNT(*) AS n FROM orders o JOIN customers c ON o.cust_id = c.id JOIN products p ON o.product_id = p.id WHERE p.category = 'Clothing' GROUP BY c.name ORDER BY n DESC, c.name LIMIT 10",
      "J4": "SELECT count(*) FROM orders o LEFT JOIN customers c ON o.cust_id = c.id WHERE c.id IS NULL",
      "J6": "SELECT COUNT(*), COUNT(o.id), COUNT(c.id) FROM orders o FULL JOIN customers c ON o.cust_id = c.id"
    };

    let editorInstance = null;
    let latestData = null;

    // Initialize CodeMirror editor
    function initEditor() {
      const textarea = document.getElementById("sqlInput");
      if (typeof CodeMirror !== "undefined") {
        editorInstance = CodeMirror.fromTextArea(textarea, {
          mode: "text/x-sql",
          theme: "dracula",
          lineNumbers: true,
          matchBrackets: true,
          autoCloseBrackets: true,
          indentUnit: 2,
          tabSize: 2,
          lineWrapping: true,
          extraKeys: {
            "Ctrl-Enter": function() { runQuery(); },
            "Cmd-Enter": function() { runQuery(); },
            "Ctrl-Space": "autocomplete",
            "Tab": function(cm) {
              if (cm.somethingSelected()) {
                cm.indentSelection("add");
              } else {
                cm.replaceSelection("  ", "end");
              }
            }
          },
          hintOptions: {
            tables: {
              "t": ["id", "city", "region", "qty", "amount", "note"],
              "orders": ["id", "cust_id", "product_id", "qty", "amount", "status"],
              "customers": ["id", "name", "region", "code", "note"],
              "products": ["id", "name", "price", "category"]
            }
          }
        });
      } else {
        // Fallback for plain textarea
        textarea.addEventListener("keydown", function(e) {
          if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
            e.preventDefault();
            runQuery();
          }
        });
      }
    }

    function getSQL() {
      if (editorInstance) {
        return editorInstance.getValue().trim();
      }
      return document.getElementById("sqlInput").value.trim();
    }

    function setSQL(sql) {
      if (editorInstance) {
        editorInstance.setValue(sql);
      } else {
        document.getElementById("sqlInput").value = sql;
      }
    }

    function insertSnippet(text) {
      if (editorInstance) {
        const doc = editorInstance.getDoc();
        const cursor = doc.getCursor();
        doc.replaceRange(text + " ", cursor);
        editorInstance.focus();
      } else {
        const textarea = document.getElementById("sqlInput");
        textarea.value += " " + text;
      }
    }

    function loadPreset() {
      const val = document.getElementById("presetSelect").value;
      if (PRESETS[val]) {
        setSQL(PRESETS[val]);
        runQuery();
      }
    }

    function onDatasetChange() {
      runQuery();
    }

    function toggleSchemaDrawer() {
      const drawer = document.getElementById("schemaDrawer");
      drawer.classList.toggle("hidden");
    }

    function clearEditor() {
      setSQL("");
      if (editorInstance) editorInstance.focus();
    }

    function copySQL() {
      const sql = getSQL();
      navigator.clipboard.writeText(sql).then(() => {
        showToast("SQL copied to clipboard", "📋");
      });
    }

    function formatSQL() {
      let sql = getSQL();
      if (!sql) return;

      const keywords = [
        "SELECT", "FROM", "WHERE", "GROUP BY", "ORDER BY", "HAVING", "LIMIT",
        "JOIN", "INNER JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL JOIN", "CROSS JOIN",
        "ON", "AS", "DISTINCT", "AND", "OR", "NOT", "IN", "BETWEEN", "LIKE",
        "IS NULL", "IS NOT NULL", "ASC", "DESC", "COUNT", "SUM", "AVG", "MIN", "MAX"
      ];

      keywords.forEach(kw => {
        const regex = new RegExp("\\b" + kw + "\\b", "gi");
        sql = sql.replace(regex, kw);
      });

      // Simple clause line breaks
      const majorClauses = ["FROM", "WHERE", "GROUP BY", "ORDER BY", "LIMIT"];
      majorClauses.forEach(cl => {
        const regex = new RegExp("\\s+(" + cl + ")\\b", "g");
        sql = sql.replace(regex, "\n$1");
      });

      setSQL(sql);
      showToast("SQL formatted", "✨");
    }

    async function runQuery() {
      const sql = getSQL();
      if (!sql) {
        showToast("Please enter a SQL query", "⚠️");
        return;
      }

      const dataset = document.getElementById("datasetSelect").value;
      const btn = document.getElementById("runBtn");
      const icon = document.getElementById("runBtnIcon");
      const text = document.getElementById("runBtnText");
      const statusText = document.getElementById("runStatusText");

      btn.disabled = true;
      icon.innerHTML = `<svg class="animate-spin h-4 w-4 text-white inline" xmlns="http://www.w3.org/2000/svg" fill="none" viewBox="0 0 24 24"><circle class="opacity-25" cx="12" cy="12" r="10" stroke="currentColor" stroke-width="4"></circle><path class="opacity-75" fill="currentColor" d="M4 12a8 8 0 018-8v8H4z"></path></svg>`;
      text.innerText = "Executing...";
      statusText.innerText = "Running query in C++ engine and SQLite...";

      document.getElementById("errorAlert").classList.add("hidden");

      try {
        const res = await fetch("/api/execute", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sql, dataset })
        });
        const data = await res.json();
        latestData = data;
        renderResults(data);
      } catch (err) {
        showError("Network or execution failure: " + err);
      } finally {
        btn.disabled = false;
        icon.innerText = "▶";
        text.innerText = "Run & Compare";
        statusText.innerText = "";
      }
    }

    async function explainPlan() {
      const sql = getSQL();
      if (!sql) return;

      const dataset = document.getElementById("datasetSelect").value;
      switchView("explain");
      const explainPre = document.getElementById("explainOutput");
      explainPre.innerText = "Computing query execution plan...";

      try {
        const res = await fetch("/api/explain", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ sql, dataset })
        });
        const data = await res.json();
        if (data.error) {
          explainPre.innerText = "Error generating plan:\n" + data.error;
        } else {
          explainPre.innerText = data.plan || "Empty plan";
        }
      } catch (err) {
        explainPre.innerText = "Failed to explain plan: " + err;
      }
    }

    function renderResults(data) {
      if (data.error) {
        showError(data.error);
        return;
      }

      if (data.engine && data.engine.error) {
        showError("Query Engine Error: " + data.engine.error);
      }
      if (data.sqlite && data.sqlite.error) {
        showError("SQLite Oracle Error: " + data.sqlite.error);
      }

      document.getElementById("statsBar").classList.remove("hidden");
      document.getElementById("resultsContainer").classList.remove("hidden");

      const engMs = data.engine.elapsed_ms;
      const sqMs = data.sqlite.elapsed_ms;

      // 1. Engine Latency Card
      document.getElementById("engTime").innerText = engMs < 1.0 ? engMs.toFixed(3) + " ms" : engMs.toFixed(2) + " ms";
      document.getElementById("engRows").innerText = data.engine.rows.length + " rows returned";
      document.getElementById("engHeaderTime").innerText = "(" + engMs.toFixed(2) + " ms)";

      // 2. SQLite Latency Card
      document.getElementById("sqTime").innerText = sqMs < 1.0 ? sqMs.toFixed(3) + " ms" : sqMs.toFixed(2) + " ms";
      document.getElementById("sqRows").innerText = data.sqlite.rows.length + " rows returned";
      document.getElementById("sqHeaderTime").innerText = "(" + sqMs.toFixed(2) + " ms)";

      // 3. Speedup Metric Card
      const speedup = sqMs / Math.max(engMs, 0.001);
      const speedupEl = document.getElementById("speedup");
      const speedupLabel = document.getElementById("speedupLabel");
      const speedupPct = document.getElementById("speedupPct");
      const speedupIcon = document.getElementById("speedupIcon");
      const speedupBarBottom = document.getElementById("speedupBarBottom");

      speedupEl.innerText = speedup.toFixed(2) + "x";

      if (speedup >= 1.0) {
        speedupEl.className = "text-3xl font-extrabold text-emerald-400 mt-1.5 code-font";
        speedupLabel.innerText = "FASTER than SQLite";
        speedupLabel.className = "text-emerald-400 font-bold uppercase text-[11px]";
        const pctMore = ((speedup - 1.0) * 100).toFixed(1);
        speedupPct.innerText = "+" + pctMore + "% speedup";
        speedupPct.className = "text-emerald-400 font-semibold";
        speedupIcon.innerText = "🚀";
        speedupBarBottom.className = "absolute bottom-0 left-0 right-0 h-1 bg-emerald-500";
      } else {
        speedupEl.className = "text-3xl font-extrabold text-amber-400 mt-1.5 code-font";
        speedupLabel.innerText = "Parity / SQLite fast";
        speedupLabel.className = "text-amber-400 font-bold uppercase text-[11px]";
        speedupPct.innerText = "Process overhead";
        speedupPct.className = "text-amber-400 font-semibold";
        speedupIcon.innerText = "⚡";
        speedupBarBottom.className = "absolute bottom-0 left-0 right-0 h-1 bg-amber-500";
      }

      // 4. Correctness Verification Card
      const badge = document.getElementById("matchBadge");
      const desc = document.getElementById("matchDesc");
      const matchBarBottom = document.getElementById("matchBarBottom");

      if (data.matched) {
        badge.innerHTML = `<span class="text-emerald-400">✓</span> EXACT MATCH`;
        badge.className = "text-xl font-extrabold text-emerald-400 mt-2 flex items-center gap-1.5";
        desc.innerText = "All " + data.engine.rows.length + " rows match SQLite oracle";
        desc.title = desc.innerText;
        matchBarBottom.className = "absolute bottom-0 left-0 right-0 h-1 bg-emerald-500";
      } else {
        badge.innerHTML = `<span class="text-rose-400">✗</span> MISMATCH`;
        badge.className = "text-xl font-extrabold text-rose-400 mt-2 flex items-center gap-1.5";
        desc.innerText = data.diff_reason || "Outputs differ between engine and SQLite";
        desc.title = desc.innerText;
        matchBarBottom.className = "absolute bottom-0 left-0 right-0 h-1 bg-rose-500";
      }

      // 5. Relative Latency Comparison Bar
      const totalTime = engMs + sqMs;
      const engPct = Math.max(5, Math.min(95, (engMs / Math.max(totalTime, 0.001)) * 100));
      const sqPct = 100 - engPct;
      document.getElementById("barEng").style.width = engPct + "%";
      document.getElementById("barSq").style.width = sqPct + "%";
      document.getElementById("barEngVal").innerText = engMs.toFixed(2) + " ms (" + engPct.toFixed(0) + "%)";
      document.getElementById("barSqVal").innerText = sqMs.toFixed(2) + " ms (" + sqPct.toFixed(0) + "%)";

      document.getElementById("totalRowsBadge").innerText = "Returned: " + data.engine.rows.length + " Engine / " + data.sqlite.rows.length + " SQLite rows";

      // 6. Render Data Tables
      renderTable("engTable", data.engine.header, data.engine.rows);
      renderTable("sqTable", data.sqlite.header, data.sqlite.rows);
      renderDiffTable(data);
    }

    function renderTable(elementId, headers, rows) {
      const tbl = document.getElementById(elementId);
      tbl.innerHTML = "";

      if (!headers || headers.length === 0) {
        tbl.innerHTML = "<tbody><tr class='text-slate-500 italic'><td class='p-3 text-center'>Empty result set (0 columns)</td></tr></tbody>";
        return;
      }

      let headerHtml = "<thead class='bg-slate-950/90 sticky top-0 border-b border-slate-800 z-10 text-[11px] uppercase tracking-wider text-slate-400'><tr>";
      headerHtml += `<th class="p-2 border-r border-slate-800 text-slate-600 text-center w-10">#</th>`;
      headers.forEach(h => {
        headerHtml += `<th class="p-2 text-slate-300 font-semibold whitespace-nowrap">${escapeHtml(h)}</th>`;
      });
      headerHtml += "</tr></thead>";

      let bodyHtml = "<tbody class='divide-y divide-slate-800/60 text-slate-300 text-xs'>";
      const maxRows = Math.min(rows.length, 250);
      for (let i = 0; i < maxRows; ++i) {
        bodyHtml += "<tr class='hover:bg-slate-800/40 transition duration-75'>";
        bodyHtml += `<td class="p-2 border-r border-slate-800/60 text-slate-600 text-center font-mono text-[11px] select-none">${i + 1}</td>`;
        rows[i].forEach(val => {
          if (val === "" || val === null) {
            bodyHtml += `<td class="p-2 whitespace-nowrap"><span class="badge-null">NULL</span></td>`;
          } else {
            bodyHtml += `<td class="p-2 whitespace-nowrap">${escapeHtml(String(val))}</td>`;
          }
        });
        bodyHtml += "</tr>";
      }

      if (rows.length > 250) {
        bodyHtml += `<tr><td colspan="${headers.length + 1}" class="p-3 text-slate-500 italic text-center bg-slate-950/50">... showing first 250 of ${rows.length} rows</td></tr>`;
      } else if (rows.length === 0) {
        bodyHtml += `<tr><td colspan="${headers.length + 1}" class="p-3 text-slate-500 italic text-center">0 rows returned</td></tr>`;
      }
      bodyHtml += "</tbody>";

      tbl.innerHTML = headerHtml + bodyHtml;
    }

    function renderDiffTable(data) {
      const tbl = document.getElementById("diffTable");
      tbl.innerHTML = "";

      const engRows = data.engine.rows;
      const sqRows = data.sqlite.rows;
      const headers = data.engine.header.length ? data.engine.header : data.sqlite.header;

      const summaryBadge = document.getElementById("diffSummaryBadge");
      if (data.matched) {
        summaryBadge.innerText = "✓ 100% Matching (" + engRows.length + " rows)";
        summaryBadge.className = "text-xs font-mono text-emerald-400";
      } else {
        summaryBadge.innerText = "✗ Mismatch Detected";
        summaryBadge.className = "text-xs font-mono text-rose-400";
      }

      let headerHtml = "<thead class='bg-slate-950/90 sticky top-0 border-b border-slate-800 z-10 text-[11px] text-slate-400'><tr>";
      headerHtml += `<th class="p-2 text-center w-12 border-r border-slate-800">#</th>`;
      headerHtml += `<th class="p-2 text-center w-16 border-r border-slate-800">Status</th>`;
      headers.forEach(h => {
        headerHtml += `<th class="p-2 border-r border-slate-800 text-sky-400 whitespace-nowrap">Engine: ${escapeHtml(h)}</th>`;
        headerHtml += `<th class="p-2 border-r border-slate-800 text-indigo-400 whitespace-nowrap">SQLite: ${escapeHtml(h)}</th>`;
      });
      headerHtml += "</tr></thead>";

      let bodyHtml = "<tbody class='divide-y divide-slate-800/60 text-xs'>";
      const maxDisplay = Math.min(Math.max(engRows.length, sqRows.length), 200);

      for (let i = 0; i < maxDisplay; ++i) {
        const eRow = i < engRows.length ? engRows[i] : null;
        const sRow = i < sqRows.length ? sqRows[i] : null;

        let rowMatches = true;
        if (!eRow || !sRow || eRow.length !== sRow.length) {
          rowMatches = false;
        } else {
          for (let j = 0; j < eRow.length; ++j) {
            if (eRow[j] !== sRow[j]) {
              // Float check tolerance
              const fe = parseFloat(eRow[j]);
              const fs = parseFloat(sRow[j]);
              if (!isNaN(fe) && !isNaN(fs) && Math.abs(fe - fs) < 1e-6) {
                // equal
              } else {
                rowMatches = false;
                break;
              }
            }
          }
        }

        const trClass = rowMatches ? "hover:bg-slate-800/30" : "bg-rose-950/30 hover:bg-rose-950/50";
        bodyHtml += `<tr class="${trClass}">`;
        bodyHtml += `<td class="p-2 text-center font-mono text-slate-600 border-r border-slate-800/60">${i + 1}</td>`;
        bodyHtml += `<td class="p-2 text-center border-r border-slate-800/60">${rowMatches ? '<span class="text-emerald-400 font-bold">✓</span>' : '<span class="text-rose-400 font-bold">≠</span>'}</td>`;

        for (let j = 0; j < headers.length; ++j) {
          const ev = eRow && j < eRow.length ? eRow[j] : "";
          const sv = sRow && j < sRow.length ? sRow[j] : "";
          const cellMatches = ev === sv || (!isNaN(parseFloat(ev)) && !isNaN(parseFloat(sv)) && Math.abs(parseFloat(ev) - parseFloat(sv)) < 1e-6);

          const cellClass = cellMatches ? "" : "bg-rose-500/20 text-rose-200 font-semibold";
          bodyHtml += `<td class="p-2 border-r border-slate-800/60 whitespace-nowrap ${cellClass}">${ev === "" ? '<span class="badge-null">NULL</span>' : escapeHtml(String(ev))}</td>`;
          bodyHtml += `<td class="p-2 border-r border-slate-800/60 whitespace-nowrap ${cellClass}">${sv === "" ? '<span class="badge-null">NULL</span>' : escapeHtml(String(sv))}</td>`;
        }
        bodyHtml += `</tr>`;
      }

      bodyHtml += "</tbody>";
      tbl.innerHTML = headerHtml + bodyHtml;
    }

    function switchView(viewName) {
      const btnSide = document.getElementById("tabSideBySide");
      const btnDiff = document.getElementById("tabDiff");
      const btnExplain = document.getElementById("tabExplain");

      const viewSide = document.getElementById("viewSideBySide");
      const viewDiff = document.getElementById("viewDiff");
      const viewExplain = document.getElementById("viewExplain");

      // Reset tabs
      [btnSide, btnDiff, btnExplain].forEach(btn => {
        btn.className = "text-xs font-semibold px-3 py-1.5 rounded-lg bg-slate-800 text-slate-400 hover:text-slate-200 border border-slate-700 transition flex items-center gap-1.5";
      });
      [viewSide, viewDiff, viewExplain].forEach(v => v.classList.add("hidden"));

      if (viewName === "sideBySide") {
        btnSide.className = "text-xs font-semibold px-3 py-1.5 rounded-lg bg-sky-500/20 text-sky-400 border border-sky-500/30 transition flex items-center gap-1.5";
        viewSide.classList.remove("hidden");
      } else if (viewName === "diff") {
        btnDiff.className = "text-xs font-semibold px-3 py-1.5 rounded-lg bg-sky-500/20 text-sky-400 border border-sky-500/30 transition flex items-center gap-1.5";
        viewDiff.classList.remove("hidden");
      } else if (viewName === "explain") {
        btnExplain.className = "text-xs font-semibold px-3 py-1.5 rounded-lg bg-sky-500/20 text-sky-400 border border-sky-500/30 transition flex items-center gap-1.5";
        viewExplain.classList.remove("hidden");
      }
    }

    function filterTable(tableId, query) {
      const q = query.toLowerCase().trim();
      const rows = document.querySelectorAll("#" + tableId + " tbody tr");
      rows.forEach(r => {
        if (!q) {
          r.style.display = "";
        } else {
          const text = r.innerText.toLowerCase();
          r.style.display = text.includes(q) ? "" : "none";
        }
      });
    }

    function copyTableCSV(source) {
      if (!latestData) return;
      const d = source === "eng" ? latestData.engine : latestData.sqlite;
      if (!d || !d.header) return;

      let csvText = d.header.join(",") + "\n";
      d.rows.forEach(row => {
        const formatted = row.map(cell => {
          const s = String(cell);
          if (s.includes(",") || s.includes("\"") || s.includes("\n")) {
            return `"${s.replace(/"/g, '""')}"`;
          }
          return s;
        });
        csvText += formatted.join(",") + "\n";
      });

      navigator.clipboard.writeText(csvText).then(() => {
        showToast((source === "eng" ? "Engine" : "SQLite") + " CSV copied to clipboard", "📋");
      });
    }

    function showError(msg) {
      const alert = document.getElementById("errorAlert");
      document.getElementById("errorMsg").innerText = msg;
      alert.classList.remove("hidden");
    }

    function showToast(msg, icon = "ℹ️") {
      const toast = document.getElementById("toast");
      document.getElementById("toastMsg").innerText = msg;
      document.getElementById("toastIcon").innerText = icon;
      toast.classList.remove("translate-y-12", "opacity-0");
      setTimeout(() => {
        toast.classList.add("translate-y-12", "opacity-0");
      }, 2500);
    }

    function escapeHtml(str) {
      if (str === null || str === undefined) return "";
      return String(str)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/"/g, "&quot;")
        .replace(/'/g, "&#039;");
    }

    window.onload = function() {
      initEditor();
      runQuery();
    };
  </script>
</body>
</html>
"""


class DatabaseManager:
    """Manages cached in-memory SQLite instances so query execution is measured instantly."""
    def __init__(self, repo_root):
        self.repo_root = repo_root
        self.data_dir = os.path.join(repo_root, "tests", "data")
        self.join_dir = os.path.join(self.data_dir, "join_data")
        self._connections = {}
        self._lock = threading.Lock()

    def get_connection(self, dataset="100k"):
        with self._lock:
            if dataset in self._connections:
                return self._connections[dataset]

            conn = sqlite3.connect(":memory:", check_same_thread=False)

            # Single table dataset
            if dataset == "100k":
                t_csv = os.path.join(self.data_dir, "data_100k.csv")
            elif dataset == "5m":
                t_csv = os.path.join(self.data_dir, "data_5m.csv")
            else:
                t_csv = os.path.join(self.data_dir, "data_small.csv")

            if os.path.exists(t_csv):
                load_csv_into_sqlite(t_csv, conn, "t")

            # Load multi-table join datasets if available
            orders_csv = os.path.join(self.join_dir, "orders.csv")
            customers_csv = os.path.join(self.join_dir, "customers.csv")
            products_csv = os.path.join(self.join_dir, "products.csv")

            if os.path.exists(orders_csv):
                load_csv_into_sqlite(orders_csv, conn, "orders")
            if os.path.exists(customers_csv):
                load_csv_into_sqlite(customers_csv, conn, "customers")
            if os.path.exists(products_csv):
                load_csv_into_sqlite(products_csv, conn, "products")

            self._connections[dataset] = conn
            return conn


class SideBySideHandler(http.server.BaseHTTPRequestHandler):
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    engine_bin = os.path.join(repo_root, "engine")
    data_dir = os.path.join(repo_root, "tests", "data")
    db_manager = None

    def log_message(self, format, *args):
        # Concise logging
        sys.stderr.write(f"[{self.log_date_time_string()}] {args[0]} {args[1]}\n")

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path in ["/", "/index.html"]:
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers()
            self.wfile.write(HTML_TEMPLATE.encode("utf-8"))
        elif parsed.path == "/api/schema":
            self.send_schema()
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        content_length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(content_length).decode("utf-8") if content_length > 0 else "{}"
        try:
            req_data = json.loads(body)
        except Exception:
            req_data = {}

        if parsed.path == "/api/execute":
            self.handle_execute(req_data)
        elif parsed.path == "/api/explain":
            self.handle_explain(req_data)
        else:
            self.send_response(404)
            self.end_headers()

    def send_schema(self):
        schema = {
            "tables": {
                "t": [
                    {"name": "id", "type": "INT"},
                    {"name": "city", "type": "STRING"},
                    {"name": "region", "type": "STRING"},
                    {"name": "qty", "type": "INT"},
                    {"name": "amount", "type": "FLOAT"},
                    {"name": "note", "type": "STRING"}
                ],
                "orders": [
                    {"name": "id", "type": "INT"},
                    {"name": "cust_id", "type": "INT"},
                    {"name": "product_id", "type": "INT"},
                    {"name": "qty", "type": "INT"},
                    {"name": "amount", "type": "FLOAT"},
                    {"name": "status", "type": "STRING"}
                ],
                "customers": [
                    {"name": "id", "type": "INT"},
                    {"name": "name", "type": "STRING"},
                    {"name": "region", "type": "STRING"},
                    {"name": "code", "type": "STRING"},
                    {"name": "note", "type": "STRING"}
                ],
                "products": [
                    {"name": "id", "type": "INT"},
                    {"name": "name", "type": "STRING"},
                    {"name": "price", "type": "FLOAT"},
                    {"name": "category", "type": "STRING"}
                ]
            }
        }
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(schema).encode("utf-8"))

    def build_engine_table_args(self, sql, dataset):
        sql_lower = sql.lower()
        table_args = []

        # Check multi-table joins
        jdir = os.path.join(self.data_dir, "join_data")
        if any(tbl in sql_lower for tbl in ["orders", "customers", "products"]):
            table_args.extend([
                "--table", f"orders={os.path.join(jdir, 'orders.csv')}",
                "--table", f"customers={os.path.join(jdir, 'customers.csv')}",
                "--table", f"products={os.path.join(jdir, 'products.csv')}",
            ])

        # Single table t
        if "t" in sql_lower or not table_args:
            if dataset == "100k":
                csv_path = os.path.join(self.data_dir, "data_100k.csv")
            elif dataset == "5m":
                csv_path = os.path.join(self.data_dir, "data_5m.csv")
            else:
                csv_path = os.path.join(self.data_dir, "data_small.csv")
            table_args.extend(["--table", f"t={csv_path}"])

        return table_args

    def handle_explain(self, req_data):
        sql = req_data.get("sql", "").strip()
        dataset = req_data.get("dataset", "100k")

        table_args = self.build_engine_table_args(sql, dataset)
        cmd = [self.engine_bin] + table_args + ["--sql", sql, "--explain"]

        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=10)
            if p.returncode == 0:
                resp = {"plan": p.stdout.strip(), "error": None}
            else:
                resp = {"plan": None, "error": p.stderr.strip() or f"Process exited with code {p.returncode}"}
        except Exception as e:
            resp = {"plan": None, "error": str(e)}

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))

    def handle_execute(self, req_data):
        sql = req_data.get("sql", "").strip()
        dataset = req_data.get("dataset", "100k")

        if not sql:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(json.dumps({"error": "Empty SQL query"}).encode("utf-8"))
            return

        table_args = self.build_engine_table_args(sql, dataset)

        # ---------------------------------------------------------------------
        # 1. Execute Query Engine (C++17)
        # ---------------------------------------------------------------------
        cmd = [self.engine_bin] + table_args + ["--sql", sql]
        eng_header = []
        eng_rows = []
        eng_error = None

        t0 = time.perf_counter()
        try:
            p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=30)
            t1 = time.perf_counter()
            eng_ms = (t1 - t0) * 1000.0

            if p.returncode != 0:
                eng_error = p.stderr.strip() or f"Engine exited with status {p.returncode}"
            else:
                reader = csv.reader(io.StringIO(p.stdout))
                all_r = list(reader)
                if all_r:
                    eng_header = all_r[0]
                    num_cols = len(eng_header)
                    for r in all_r[1:]:
                        if len(r) == 0 and num_cols == 1:
                            eng_rows.append([""])
                        else:
                            eng_rows.append(r)
        except subprocess.TimeoutExpired:
            t1 = time.perf_counter()
            eng_ms = (t1 - t0) * 1000.0
            eng_error = "Engine execution timed out after 30 seconds"
        except Exception as e:
            t1 = time.perf_counter()
            eng_ms = (t1 - t0) * 1000.0
            eng_error = str(e)

        # ---------------------------------------------------------------------
        # 2. Execute SQLite Oracle
        # ---------------------------------------------------------------------
        sq_header = []
        raw_sq = []
        sq_rows = []
        sq_error = None
        sq_ms = 0.0

        try:
            conn = self.db_manager.get_connection(dataset)
            cur = conn.cursor()
            t0 = time.perf_counter()
            cur.execute(sql)
            raw_sq = cur.fetchall()
            t1 = time.perf_counter()
            sq_ms = (t1 - t0) * 1000.0

            sq_header = [d[0] for d in cur.description] if cur.description else []
            # Convert for JSON serialization after comparison
            sq_rows = [[str(v) if v is not None else "" for v in row] for row in raw_sq]
        except Exception as e:
            sq_error = str(e)

        # ---------------------------------------------------------------------
        # 3. Correctness Verification & Matching
        # ---------------------------------------------------------------------
        matched = False
        diff_reason = None
        is_ordered = bool(re.search(r"\border\s+by\b", sql, re.IGNORECASE))

        if eng_error:
            matched = False
            diff_reason = f"Engine error: {eng_error}"
        elif sq_error:
            matched = False
            diff_reason = f"SQLite error: {sq_error}"
        elif len(eng_rows) != len(raw_sq):
            matched = False
            diff_reason = f"Row count differs: Engine returned {len(eng_rows)} rows, SQLite returned {len(raw_sq)} rows"
        elif len(eng_header) != len(sq_header):
            matched = False
            diff_reason = f"Column count differs: Engine returned {len(eng_header)} columns, SQLite returned {len(sq_header)} columns"
        else:
            # Check row values using raw SQLite types (int, float, None, str) for exact tolerance & NULL matching
            e_compare = eng_rows
            s_compare = raw_sq
            if not is_ordered and len(eng_rows) > 0:
                e_compare = sorted(eng_rows, key=make_row_sort_key)
                s_compare = sorted(raw_sq, key=make_row_sort_key)

            mismatch_found = False
            for r_idx, (er, sr) in enumerate(zip(e_compare, s_compare)):
                if not rows_match(er, sr):
                    mismatch_found = True
                    for c_idx, (ec, sc) in enumerate(zip(er, sr)):
                        if not cells_match(ec, sc):
                            col_name = eng_header[c_idx] if c_idx < len(eng_header) else f"col_{c_idx}"
                            diff_reason = f"Value mismatch at row {r_idx + 1}, column '{col_name}': Engine='{ec}', SQLite='{sc}'"
                            break
                    break

            if not mismatch_found:
                matched = True
                diff_reason = None
            else:
                matched = False

        # Speedup ratio
        speedup = sq_ms / max(eng_ms, 0.001)

        resp = {
            "matched": matched,
            "diff_reason": diff_reason,
            "is_ordered": is_ordered,
            "speedup": round(speedup, 2),
            "engine": {
                "elapsed_ms": eng_ms,
                "header": eng_header,
                "rows": eng_rows,
                "error": eng_error
            },
            "sqlite": {
                "elapsed_ms": sq_ms,
                "header": sq_header,
                "rows": sq_rows,
                "error": sq_error
            }
        }

        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(resp).encode("utf-8"))


def main():
    port = 8080
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except ValueError:
            pass

    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    SideBySideHandler.db_manager = DatabaseManager(repo_root)

    print(f"==================================================================")
    print(f"  SQL Query Engine - Interactive Side-by-Side Runner")
    print(f"  Listening at: http://localhost:{port}")
    print(f"  Press Ctrl+C to stop.")
    print(f"==================================================================")

    # Preload default 100k SQLite database in background thread for instant response
    threading.Thread(target=SideBySideHandler.db_manager.get_connection, args=("100k",), daemon=True).start()

    server = http.server.HTTPServer(("0.0.0.0", port), SideBySideHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server.")
        server.server_close()


if __name__ == "__main__":
    main()
