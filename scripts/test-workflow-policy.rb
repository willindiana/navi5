#!/usr/bin/env ruby
# Navi5 — offline master workflow safety checks.
# SPDX-License-Identifier: GPL-3.0-or-later
require 'yaml'
root = File.expand_path('..', __dir__)
load_workflow = ->(path) { YAML.load_file(File.join(root, path)) }
build = load_workflow.call('.forgejo/workflows/release-build.yml')
store = load_workflow.call('.github/workflows/store-verify.yml')
[build, store].each do |workflow|
  events = workflow.fetch('on', workflow[true]) # Psych YAML 1.1 reads on as true.
  raise 'unexpected event' unless (events.keys - %w[push pull_request workflow_dispatch]).empty?
  raise 'manual dispatch missing' unless events.key?('workflow_dispatch')
  events.each do |event, config|
    next if event == 'workflow_dispatch'
    raise 'wrong base' unless config.fetch('branches') == ['master']
  end
  workflow.fetch('jobs').each_value do |job|
    raise 'runner changed' unless job.fetch('runs-on') == 'ubuntu-24.04'
    checkout = job.fetch('steps').find { |step| step['uses']&.include?('actions/checkout@') }
    raise 'checkout credentials persisted' unless checkout.fetch('with').fetch('persist-credentials') == false
    raise 'beta guard retained' if job.fetch('if').include?('release/beta')
    raise 'master guard missing' unless job.fetch('if').include?('refs/heads/master')
  end
end
raise 'Forgejo PR check missing' unless build.fetch('on', build[true]).key?('pull_request')
raise 'store not manual-only' unless store.fetch('on', store[true]).keys == ['workflow_dispatch']
[build, store].each do |workflow|
  raise 'write permission' unless workflow.fetch('permissions') == {'contents' => 'read'}
end
puts 'Master triggers, Forgejo PR build, manual store gate and read-only checkout passed'
