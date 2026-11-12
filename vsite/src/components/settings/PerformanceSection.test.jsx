import { afterEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { PerformanceSection } from './PerformanceSection';

const makeGpu = (overrides = {}) => ({
  use_gpu: false,
  cuda_available: true,
  mps_available: false,
  accelerators: ['cuda'],
  device: 'cuda',
  device_label: 'CUDA (NVIDIA)',
  batch_size: 0,
  effective_batch_size: 16,
  precision_mode: 'auto',
  active_precision: 'fp32',
  fp16_supported: true,
  gpu_name: '',
  ...overrides,
});

const makeProps = (overrides = {}) => ({
  gpuSettings: makeGpu(),
  status: {},
  uiTheme: 'modern',
  isSwitchingGpu: false,
  handleToggleGpu: vi.fn(),
  handleUpdatePerformanceSettings: vi.fn(),
  ...overrides,
});

describe('PerformanceSection', () => {
  afterEach(cleanup);

  describe('GPU toggle', () => {
    it.each(['modern', 'terminal'])('is disabled without any accelerator and shows the warning (%s)', (uiTheme) => {
      render(<PerformanceSection {...makeProps({ uiTheme, gpuSettings: makeGpu({ cuda_available: false, accelerators: [], device: 'cpu', device_label: 'CPU' }) })} />);
      expect(screen.getByText('No GPU accelerator detected — running on CPU')).toBeTruthy();
      expect(screen.getByText(/No GPU accelerator \(CUDA or MPS\) is available/)).toBeTruthy();
      expect(screen.getByLabelText('Enable GPU acceleration').disabled).toBe(true);
    });

    it.each(['modern', 'terminal'])('calls handleToggleGpu (%s)', (uiTheme) => {
      const props = makeProps({ uiTheme });
      render(<PerformanceSection {...props} />);
      expect(screen.queryByText(/No GPU accelerator \(CUDA or MPS\) is available/)).toBeNull();
      expect(screen.getByText('CUDA detected — enable GPU acceleration')).toBeTruthy();
      fireEvent.click(screen.getByLabelText('Enable GPU acceleration'));
      expect(props.handleToggleGpu).toHaveBeenCalledTimes(1);
    });

    it('accepts an MPS-only machine', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ cuda_available: false, mps_available: true, accelerators: ['mps'], device: 'cpu', device_label: 'CPU' }) })} />);
      const toggle = screen.getByLabelText('Enable GPU acceleration');
      expect(toggle.disabled).toBe(false);
      expect(screen.getByText('MPS detected — enable GPU acceleration')).toBeTruthy();
    });

    it('lists every available accelerator', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ mps_available: true, accelerators: ['cuda', 'mps'] }) })} />);
      expect(screen.getByText('CUDA / MPS detected — enable GPU acceleration')).toBeTruthy();
    });

    it('describes the device in use', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ use_gpu: true, device_label: 'RTX 4090' }) })} />);
      expect(screen.getByText('Using RTX 4090')).toBeTruthy();
      expect(screen.getByLabelText('Disable GPU acceleration')).toBeTruthy();
    });

    it('is disabled while switching', () => {
      render(<PerformanceSection {...makeProps({ isSwitchingGpu: true })} />);
      expect(screen.getByLabelText('Enable GPU acceleration').disabled).toBe(true);
    });
  });

  describe('batch size and precision', () => {
    it('uses the custom select in the modern theme', () => {
      const props = makeProps();
      const { container } = render(<PerformanceSection {...props} />);
      expect(container.querySelectorAll('select')).toHaveLength(0);

      fireEvent.click(screen.getByText('Auto'));
      fireEvent.click(screen.getByText('16'));
      expect(props.handleUpdatePerformanceSettings).toHaveBeenCalledWith({ batch_size: '16' });
    });

    it('changes precision with the custom select in the modern theme', () => {
      const props = makeProps();
      render(<PerformanceSection {...props} />);
      fireEvent.click(screen.getByText('Auto (FP32)'));
      fireEvent.click(screen.getByText('FP16 (Half Precision)'));
      expect(props.handleUpdatePerformanceSettings).toHaveBeenCalledWith({ precision_mode: 'fp16' });
    });

    it('uses native selects in the terminal theme', () => {
      const props = makeProps({ uiTheme: 'terminal' });
      const { container } = render(<PerformanceSection {...props} />);
      const selects = container.querySelectorAll('select');
      expect(selects).toHaveLength(2);

      fireEvent.change(selects[0], { target: { value: '16' } });
      expect(props.handleUpdatePerformanceSettings).toHaveBeenCalledWith({ batch_size: '16' });
      fireEvent.change(selects[1], { target: { value: 'fp16' } });
      expect(props.handleUpdatePerformanceSettings).toHaveBeenCalledWith({ precision_mode: 'fp16' });
    });

    it('labels the auto precision option with the active precision', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ active_precision: 'fp16' }) })} />);
      expect(screen.getByText('Auto (FP16)')).toBeTruthy();
    });

    it('falls back to FP32 when no precision is reported', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ active_precision: '' }), uiTheme: 'terminal' })} />);
      expect(screen.getByRole('option', { name: 'Auto (FP32)' })).toBeTruthy();
    });

    it('describes auto mode and the FP16 support hint', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ effective_batch_size: 32 }) })} />);
      expect(screen.getByText('Auto mode active (Effective batch size: 32)')).toBeTruthy();
      expect(screen.getByText('FP16 compute supported')).toBeTruthy();
    });

    it('shows the manual batch hint and the legacy GPU hint', () => {
      render(<PerformanceSection {...makeProps({ gpuSettings: makeGpu({ batch_size: 8, fp16_supported: false }) })} />);
      expect(screen.getByText('Higher batch size increases GPU utilization.')).toBeTruthy();
      expect(screen.getByText('FP16 unavailable — falling back to FP32')).toBeTruthy();
    });
  });

  describe('OOM badge', () => {
    it('is hidden normally', () => {
      render(<PerformanceSection {...makeProps()} />);
      expect(screen.queryByText(/OOM \/ Memory Safety Fallback Triggered/)).toBeNull();
    });

    it.each([
      ['gpuSettings', { gpuSettings: makeGpu({ oom_triggered: true }) }],
      ['status', { status: { oom_triggered: true } }],
    ])('appears when %s reports an OOM', (_label, overrides) => {
      render(<PerformanceSection {...makeProps(overrides)} />);
      expect(screen.getByText(/OOM \/ Memory Safety Fallback Triggered/)).toBeTruthy();
    });
  });
});
